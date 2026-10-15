#pragma once

#include <Common/Settings/CapabilityCatalog.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Core/PostProcessSettings.hpp>

#include <string>
#include <vector>

// WHAT A VIEW RENDERS: THE ONE RESOLUTION OF THE THREE RENDER METHODS (Common/Settings/RenderMethods.hpp).
//
//   effective = the highest IMPLEMENTED method at or below
//               min( the view's request (PostProcessSettings, blended from its volumes),
//                    the machine's ceiling (Scalability *.Method, ResolvedQuality),
//                    what the device can run (CapabilityCatalog: ray query for the traced methods),
//                    what the render path feeds (the G-buffer: NeedsGBuffer, deferred only) )
//
// Each axis records the FIRST limit that lowered it, so a downgrade names one cause. The renderer reads
// `Effective` and nothing else; the log gets one line per distinct downgrade (ReportRenderMethods) and the
// viewport overlay draws the same rows (FormatRenderMethod). There is no other place a method is lowered.
namespace Desert::Graphic
{
    // Why a method came out below the request. The first limit met wins.
    enum class MethodLimit : int
    {
        None = 0,       // rendered as requested
        Scalability,    // the machine's ceiling (Scalability "<Group>.Method") is lower
        Device,         // the device cannot run it (no ray query in the CapabilityCatalog)
        RenderPath,     // the view renders forward: no G-buffer for a method that reads one (NeedsGBuffer)
        NotImplemented, // declared, but this build has no renderer for it (only a ceiling can name one)
    };

    template <typename Method>
    struct ResolvedMethod
    {
        Method      Requested{}; // the view's PostProcessSettings
        Method      Ceiling{};   // the machine's Scalability value
        Method      Effective{}; // what the frame renders
        MethodLimit Limit = MethodLimit::None;

        [[nodiscard]] bool Lowered() const
        {
            return Effective != Requested;
        }
        bool operator==( const ResolvedMethod& ) const = default;
    };

    struct RenderMethods
    {
        ResolvedMethod<Common::Scalability::GIMethod>         GlobalIllumination;
        ResolvedMethod<Common::Scalability::ReflectionMethod> Reflections;
        ResolvedMethod<Common::Scalability::ShadowMethod>     Shadows;
        // The shadow supplements, after the same rule (both refused at load until they land, so both false).
        bool ContactShadows       = false;
        bool DistanceFieldShadows = false;

        bool operator==( const RenderMethods& ) const = default;
    };

    // PURE: no log, no globals. `gBuffer` = the view renders this frame on the deferred path (SceneRenderer's
    // m_RenderPath, after a debug view forced forward), so a method that reads the G-buffer can run.
    [[nodiscard]] RenderMethods ResolveRenderMethods( const Core::PostProcessSettings&              post,
                                                      const Common::Scalability::ResolvedQuality&   quality,
                                                      const Common::Scalability::CapabilityCatalog& catalog,
                                                      bool                                          gBuffer );

    // One row per axis: "GI: ScreenSpace" or, lowered, "GI: HardwareTraced -> ScreenSpace (device: no ray query)".
    // The log line and the overlay print exactly these.
    [[nodiscard]] std::vector<std::string> FormatRenderMethods( const RenderMethods& methods );

    // Logs each LOWERED axis once per process (every SceneRenderer resolves its own view; one cause is one line,
    // not one per viewport and frame).
    void ReportRenderMethods( const RenderMethods& methods );
} // namespace Desert::Graphic
