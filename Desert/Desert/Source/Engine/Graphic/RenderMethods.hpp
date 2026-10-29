#pragma once

#include <Common/Settings/CapabilityCatalog.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Core/PostProcessSettings.hpp>

#include <string>
#include <string_view>
#include <vector>

// WHAT A VIEW RENDERS: THE ONE RESOLUTION OF THE THREE RENDER METHODS (Common/Settings/RenderMethods.hpp).
//
//   effective = the highest IMPLEMENTED method at or below
//               min( the view's request (PostProcessSettings, blended from its volumes), REPLACED by an
//                    explicit machine.json override of *.Method (ResolvedQuality::Overridden,
//                    MethodLimit::Override), the machine's ceiling (Scalability *.Method level, ResolvedQuality),
//                    what the device can run (CapabilityCatalog: ray query for the traced methods,
//                    the ray tracing pipeline for PathTraced),
//                    what the view's renderers built on it (ProgramRefusals: a program only that method needs),
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
        Override,       // machine.json overrides "<Group>.Method": it replaces the request, up or down
        Device,         // the device cannot run it (no ray query in the CapabilityCatalog)
        DevicePipeline, // the device has no ray tracing pipeline (CapabilityCatalog RayTracingPipeline):
                        // PathTraced
        PathTracerPair, // PathTraced on one of GI / reflections only: the path tracer is one integrator for both
        RenderPath,     // the view renders forward: no G-buffer for a method that reads one (NeedsGBuffer)
        NotImplemented, // declared, but this build has no renderer for it (only a ceiling can name one)
        FrameScene,     // this view-frame's ray-traced scene (TLAS, trace resources) could not be built
        RenderArea,     // the device's framebuffer / viewport is below the method's render area (NeedsRenderArea)
        FragmentStores, // the device has no stores / atomics in the fragment stage (NeedsFragmentStores)
        Program,        // a program only this method needs did not build on this device (ProgramRefusals)
    };

    template <typename Method>
    struct ResolvedMethod
    {
        Method      Requested{}; // the view's PostProcessSettings
        Method      Ceiling{};   // the machine's Scalability value (a level's ceiling, or an override's choice)
        Method      Effective{}; // what the frame renders
        MethodLimit Limit = MethodLimit::None;
        // Limit == Program: the program that did not build (MethodRefusal::Program, static storage).
        std::string_view Program;

        [[nodiscard]] bool Lowered() const
        {
            return Effective != Requested;
        }
        bool operator==( const ResolvedMethod& ) const = default;
    };

    // A method one of the view's renderers cannot draw on this device because a program ONLY that method needs did
    // not build (DeferredLightingRenderer: the MegaLights composite is Stochastic's alone). The method is refused
    // by name and the resolution walks on down its ladder; the renderer and every other method keep working. A
    // program every method needs failing is the renderer's own initialisation error, never a refusal.
    template <typename Method>
    struct MethodRefusal
    {
        Method           Refused{};
        std::string_view Program; // the program's debug name, static storage
        std::string      Reason;  // why it did not build, as said in the refusal's one ERROR line
    };
    // One list per axis; the view's renderers append the methods whose own program did not build.
    struct ProgramRefusals
    {
        std::vector<MethodRefusal<Common::Scalability::GIMethod>>         GlobalIllumination;
        std::vector<MethodRefusal<Common::Scalability::ReflectionMethod>> Reflections;
        std::vector<MethodRefusal<Common::Scalability::ShadowMethod>>     Shadows;
    };

    // Records @p method refused for @p program because of @p reason; false when it already was (the caller logs
    // only a new refusal).
    template <typename Method>
    bool AddRefusal( std::vector<MethodRefusal<Method>>& refusals, const Method method,
                     const std::string_view program, const std::string_view reason )
    {
        for ( const MethodRefusal<Method>& refusal : refusals )
            if ( refusal.Refused == method )
                return false;
        refusals.push_back( { .Refused = method, .Program = program, .Reason = std::string( reason ) } );
        return true;
    }
    // Whether @p refusals holds @p method refused for @p program with exactly @p reason - the same refusal again.
    template <typename Method>
    [[nodiscard]] bool HoldsRefusal( const std::vector<MethodRefusal<Method>>& refusals, const Method method,
                                     const std::string_view program, const std::string_view reason )
    {
        for ( const MethodRefusal<Method>& refusal : refusals )
            if ( refusal.Refused == method && refusal.Program == program && refusal.Reason == reason )
                return true;
        return false;
    }

    struct RenderMethods
    {
        ResolvedMethod<Common::Scalability::GIMethod>         GlobalIllumination;
        ResolvedMethod<Common::Scalability::ReflectionMethod> Reflections;
        ResolvedMethod<Common::Scalability::ShadowMethod>     Shadows;
        // Shadows Stochastic's visibility source (Common::Scalability::StochasticVisibility): the best the device
        // runs (`DeviceVisibility`) and this frame's (`Visibility`: the device's, or, a ray-query device whose frame
        // has no traceable scene, the best source without rays - LowerShadowsForFrame). Meaningless under any
        // other Shadows method.
        Common::Scalability::StochasticVisibility DeviceVisibility = Common::Scalability::StochasticVisibility::RayQuery;
        Common::Scalability::StochasticVisibility Visibility       = Common::Scalability::StochasticVisibility::RayQuery;
        // The best source without rays on this device (VirtualMapsAndScreen or Screen): where a RayQuery frame
        // without a TLAS lands.
        Common::Scalability::StochasticVisibility RaylessVisibility = Common::Scalability::StochasticVisibility::Screen;
        // The shadow supplements, after the same rule (distance-field shadows refused at load until they land).
        bool ContactShadows       = false;
        bool DistanceFieldShadows = false;

        bool operator==( const RenderMethods& ) const = default;
    };

    // PURE: no log, no globals. `gBuffer` = the view renders this frame on the deferred path (SceneRenderer's
    // m_RenderPath, after a debug view forced forward), so a method that reads the G-buffer can run. `refusals` =
    // the methods the view's renderers could not build a program for (ProgramRefusals).
    [[nodiscard]] RenderMethods ResolveRenderMethods( const Core::PostProcessSettings&              post,
                                                      const Common::Scalability::ResolvedQuality&   quality,
                                                      const Common::Scalability::CapabilityCatalog& catalog,
                                                      bool gBuffer, const ProgramRefusals& refusals );

    // THE FRAME'S SUN-SHADOW PATH, decided ONCE per view-frame before any pass is declared
    // (SceneRenderer::OnUpdate, right after the view's TLAS is declared): HardwareTraced whose frame has no
    // traceable scene (`rayTracedSceneReady` false: TLAS not built, or the trace resources could not be created)
    // is lowered here to CascadedMaps with MethodLimit::FrameScene; Stochastic keeps its method and its
    // Visibility drops from RayQuery to RaylessVisibility. Every pass of the frame (cascades, prepass, composite,
    // forward) reads one decision and the overlay / log name the cause. PURE. The next BeginScene resolves afresh.
    void LowerShadowsForFrame( RenderMethods& methods, bool rayTracedSceneReady );

    // One light's Shadow Method this frame and what it resolved to (ResolveLightVisibility); SourceId =
    // LocalLightShadowSettings::SourceId (entity + 1).
    struct StochasticLightMethod
    {
        uint32_t                                  SourceId  = 0;
        Common::Scalability::LightShadowMethod    Requested = Common::Scalability::LightShadowMethod::Auto;
        Common::Scalability::StochasticVisibility Effective = Common::Scalability::StochasticVisibility::Screen;
    };

    // THE VISIBILITY SOURCE OF ONE LIGHT'S MegaLights SAMPLES this frame (Common::Scalability::LightShadowMethod,
    // the light component's Shadow Method), after LowerShadowsForFrame: Auto is the frame's Visibility; RayQuery
    // only while the frame traces the ray query (its TLAS built), else the frame's Visibility; VirtualMaps only on a
    // device that runs them (RaylessVisibility vsm+screen), else RaylessVisibility; Screen always. PURE.
    [[nodiscard]] Common::Scalability::StochasticVisibility
    ResolveLightVisibility( Common::Scalability::LightShadowMethod requested, const RenderMethods& methods );

    // This frame's Shadows trace hardware rays: HardwareTraced, or Stochastic whose visibility is the ray query.
    [[nodiscard]] bool ShadowsTraceRays( const RenderMethods& methods );

    // THE VIEW PATH-TRACES this frame: GI and reflections both resolved to PathTraced (they are one integrator,
    // ResolveRenderMethods pairs them). The path tracer's image replaces the lit scene before post.
    [[nodiscard]] bool PathTraced( const RenderMethods& methods );

    // WHETHER THIS DEVICE CAN PATH-TRACE AT ALL, whatever a view requests: empty when it can, else the reason a
    // PathTraced request is refused - the same walk and the same words as ResolveRenderMethods' Device /
    // DevicePipeline limit ("the device offers no ray query" / "... no ray tracing pipeline"). Reader: DesertCtl
    // `state device`, so a no-RT device names its path-tracing refusal even on a scene that never asked for it.
    // PURE.
    [[nodiscard]] std::string_view PathTracingRefusal( const Common::Scalability::CapabilityCatalog& catalog );

    // As LowerShadowsForFrame, for the path tracer: a path-traced view whose frame has no traceable scene (TLAS not
    // built) is lowered here to HardwareTraced on both axes with MethodLimit::FrameScene, before any pass is
    // declared, so the frame renders - and the overlay names - one decision. PURE.
    void LowerPathTracingForFrame( RenderMethods& methods, bool rayTracedSceneReady );

    // One row per axis: "GI: ScreenSpace" or, lowered, "GI: HardwareTraced -> ScreenSpace (device: no ray query)".
    // The log line and the overlay print exactly these.
    [[nodiscard]] std::vector<std::string> FormatRenderMethods( const RenderMethods& methods );

    // Logs each LOWERED axis once per process (every SceneRenderer resolves its own view; one cause is one line,
    // not one per viewport and frame).
    void ReportRenderMethods( const RenderMethods& methods );
} // namespace Desert::Graphic
