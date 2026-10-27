#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/DisplaySettings.hpp>

#include <string>

// HDR OUTPUT — THE CONTRACT (HDR-OUTPUT, owner 2026-10-27; UE r.HDR.* / FHDRMetaData).
//
// WHO DECIDES WHAT:
//   - Common::Scalability::DisplaySettings::Output (machine.json) asks for an output; ResolveDisplayOutput accepts
//     it only from CapabilityCatalog::DisplayOutputs (no fallback).
//   - The PLATFORM reports the display's luminance (QueryDisplayLuminance): Mac — NSScreen EDR headroom
//     (maximumExtendedDynamicRangeColorComponentValue: current, potential, reference); Windows — DXGI
//     IDXGIOutput6::GetDesc1 (MinLuminance / MaxLuminance / MaxFullFrameLuminance), not implemented yet: it
//     returns that as an error, which keeps HDR off there (Windows has no HDR monitor today, owner 10-27).
//   - The SWAPCHAIN builds with the resolved output's format + colour space and publishes the result here
//     (PublishDisplayOutput) once per (re)build. It is the single writer.
//   - Readers: SceneComposite's output transform (TonemapRenderer -> headroom), the editor's display encode
//     (UI at paper white), the frame capture (an HDR swapchain is packed to its SDR rendition).
//
// UNITS. scRGB: linear Rec.709, 1.0 = SDR/paper white. On macOS EDR 1.0 is the white the user's brightness
// slider sets, and values up to Headroom are brighter than it — so the output transform targets Headroom, and
// nits are reported as PaperWhiteNits x value.
namespace Desert::Graphic
{
    // What the OS says about the display the window is on.
    struct DisplayLuminance
    {
        float       EdrHeadroom          = 1.0f; // now: the largest value the display shows above 1.0 (Mac EDR)
        float       PotentialHeadroom    = 1.0f; // at the display's brightest setting
        float       ReferenceHeadroom    = 0.0f; // in a reference preset (0 = none)
        float       PeakNits             = 0.0f; // the OS's nits figure (0 = the OS gives none: Mac)
        float       PaperWhiteNits       = 0.0f; // the OS's SDR white in nits (0 = the OS gives none: Mac)
        std::string Source;                       // which API answered, for the log
    };

    // PLATFORM. @p nativeWindow is the GLFWwindow*. An error = the OS could not be asked (no screen, not
    // implemented on this platform): the caller then refuses the HDR output, it does not invent a headroom.
    [[nodiscard]] Common::ResultStr<DisplayLuminance> QueryDisplayLuminance( void* nativeWindow );

    // What the swapchain presents with this frame.
    struct DisplayOutputState
    {
        Common::Scalability::DisplayOutput Output = Common::Scalability::DisplayOutput::SDR_sRGB;
        // The output transform's ceiling, in paper-white units (1 on SDR). PeakNits / PaperWhiteNits.
        float Headroom       = 1.0f;
        float PaperWhiteNits = 0.0f;
        float PeakNits       = 0.0f;

        [[nodiscard]] bool IsHDR() const
        {
            return Output != Common::Scalability::DisplayOutput::SDR_sRGB;
        }
    };

    // Render thread. The swapchain's last (re)build; SDR until one published.
    [[nodiscard]] const DisplayOutputState& CurrentDisplayOutput();
    void                                    PublishDisplayOutput( const DisplayOutputState& state );

    // PURE. The state an HDR output presents with: peak from the setting when > 0, else from the OS
    // (OS nits when it has them, else EdrHeadroom x paper white); paper white from the setting when > 0, else
    // the OS's. Error when neither gives a paper white or the headroom is not above 1.
    [[nodiscard]] Common::ResultStr<DisplayOutputState>
    ResolveDisplayOutputState( Common::Scalability::DisplayOutput            output,
                               const Common::Scalability::DisplaySettings& settings, const DisplayLuminance& os );
} // namespace Desert::Graphic
