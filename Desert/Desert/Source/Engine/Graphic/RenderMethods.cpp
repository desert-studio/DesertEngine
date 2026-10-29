#include "RenderMethods.hpp"

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <format>
#include <mutex>
#include <set>
#include <span>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        using namespace Common::Scalability;

        // What the device runs that a method may need: inline ray query (a pipeline device offers it too), the
        // ray tracing pipeline, fragment-stage stores and atomics, and the largest framebuffer / viewport side.
        struct DeviceOffers
        {
            bool     RayQuery       = false;
            bool     Pipeline       = false;
            bool     FragmentStores = false;
            uint32_t RenderArea     = 0;
        };

        [[nodiscard]] DeviceOffers OffersOf( const CapabilityCatalog& catalog )
        {
            return { .RayQuery = std::any_of( catalog.RayTracingModes.begin(), catalog.RayTracingModes.end(),
                                              []( RayTracingMode m ) { return m != RayTracingMode::None; } ),
                     .Pipeline = CapabilityCatalog::Offers( catalog.RayTracingModes,
                                                            RayTracingMode::RayTracingPipeline ),
                     .FragmentStores = catalog.FragmentStores,
                     .RenderArea     = catalog.MaxRenderArea };
        }

        // The program a view's renderer could not build for @p method, empty when every one it needs built.
        template <typename Method>
        [[nodiscard]] std::string_view RefusedProgram( std::span<const MethodRefusal<Method>> refusals,
                                                       Method                                 method )
        {
            const auto it =
                 std::find_if( refusals.begin(), refusals.end(),
                               [method]( const MethodRefusal<Method>& r ) { return r.Refused == method; } );
            return it == refusals.end() ? std::string_view{} : it->Program;
        }

        // Down the ladder from r.Effective past every method the device cannot run, the render path cannot feed,
        // this build does not implement or a renderer could not build a program for; the first limit met is
        // kept. The front (Off / CascadedMaps) is implemented, needs nothing and is never refused (its programs
        // failing is the renderer's initialisation error), so the walk always ends.
        template <typename Method>
        void WalkDown( ResolvedMethod<Method>& r, const DeviceOffers device, const bool gBuffer,
                       std::span<const MethodRefusal<Method>> refusals )
        {
            for ( ;; )
            {
                MethodLimit blocked = MethodLimit::None;
                if ( NeedsRayQuery( r.Effective ) && !device.RayQuery )
                    blocked = MethodLimit::Device;
                else if ( NeedsRayTracingPipeline( r.Effective ) && !device.Pipeline )
                    blocked = MethodLimit::DevicePipeline;
                else if ( NeedsFragmentStores( r.Effective ) && !device.FragmentStores )
                    blocked = MethodLimit::FragmentStores;
                else if ( NeedsRenderArea( r.Effective ) > device.RenderArea )
                    blocked = MethodLimit::RenderArea;
                else if ( NeedsGBuffer( r.Effective ) && !gBuffer )
                    blocked = MethodLimit::RenderPath;
                else if ( !IsImplemented( r.Effective ) )
                    blocked = MethodLimit::NotImplemented;
                else if ( const std::string_view program = RefusedProgram( refusals, r.Effective );
                          !program.empty() )
                {
                    blocked = MethodLimit::Program;
                    if ( r.Limit == MethodLimit::None )
                        r.Program = program;
                }
                if ( blocked == MethodLimit::None )
                    return;
                if ( r.Limit == MethodLimit::None )
                    r.Limit = blocked;
                r.Effective = static_cast<Method>( static_cast<int>( r.Effective ) - 1 );
            }
        }

        // The one rule, per axis: a Scalability LEVEL is a ceiling, min( request, ceiling ); an explicit machine
        // override of "<Group>.Method" is the user's choice and REPLACES the request (UE: the console variable set
        // by hand beats the post-process volume). Either way the result then walks down the ladder (the enum's
        // order): WalkDown, so an override still yields to the device, the render path and a refused program.
        template <typename Method>
        ResolvedMethod<Method> ResolveAxis( Method requested, Method ceiling, const bool overridden,
                                            const DeviceOffers device, const bool gBuffer,
                                            std::span<const MethodRefusal<Method>> refusals )
        {
            ResolvedMethod<Method> r{ .Requested = requested, .Ceiling = ceiling, .Effective = requested };
            if ( overridden ? ceiling != requested : ceiling < requested )
            {
                r.Effective = ceiling;
                r.Limit     = overridden ? MethodLimit::Override : MethodLimit::Scalability;
            }
            WalkDown( r, device, gBuffer, refusals );
            return r;
        }

        [[nodiscard]] bool OverriddenIn( const ResolvedQuality& quality, const Parameter parameter )
        {
            return quality.Overridden[static_cast<std::size_t>( parameter )];
        }

        // The path tracer is ONE integrator for GI and reflections: an axis that resolved to PathTraced while the
        // other did not is lowered one step and walked on, naming the pair as its limit.
        template <typename Method>
        void LowerUnpairedPathTraced( ResolvedMethod<Method>& r, const DeviceOffers device, const bool gBuffer,
                                      std::span<const MethodRefusal<Method>> refusals )
        {
            if ( r.Effective != Method::PathTraced )
                return;
            r.Effective = static_cast<Method>( static_cast<int>( Method::PathTraced ) - 1 );
            if ( r.Limit == MethodLimit::None )
                r.Limit = MethodLimit::PathTracerPair;
            WalkDown( r, device, gBuffer, refusals );
        }

        std::string_view LimitText( MethodLimit limit, std::string_view scalabilityKey, std::string& scratch,
                                    std::string_view ceiling, std::string_view program )
        {
            switch ( limit )
            {
                case MethodLimit::None:
                    return {};
                case MethodLimit::Scalability:
                    scratch = std::format( "Scalability {} is {}", scalabilityKey, ceiling );
                    return scratch;
                case MethodLimit::Override:
                    scratch = std::format( "machine override {} = {}", scalabilityKey, ceiling );
                    return scratch;
                case MethodLimit::Device:
                    return "the device offers no ray query";
                case MethodLimit::DevicePipeline:
                    return "the device offers no ray tracing pipeline";
                case MethodLimit::PathTracerPair:
                    return "the path tracer integrates GI and reflections together; the other axis is not "
                           "PathTraced";
                case MethodLimit::RenderPath:
                    return "the forward render path has no G-buffer";
                case MethodLimit::NotImplemented:
                    return "not implemented in this build";
                case MethodLimit::FrameScene:
                    return "the view's ray-traced scene was not built this frame";
                case MethodLimit::FragmentStores:
                    return "the device offers no fragmentStoresAndAtomics";
                case MethodLimit::RenderArea:
                    scratch = std::format( "the device's framebuffer / viewport is below {0}x{0}",
                                           kVirtualShadowMapResolution );
                    return scratch;
                case MethodLimit::Program:
                    scratch = std::format( "its program '{}' did not build on this device", program );
                    return scratch;
            }
            return {};
        }

        template <typename Method, std::size_t N>
        std::string FormatAxis( std::string_view label, std::string_view scalabilityKey,
                                const ResolvedMethod<Method>& m, const std::array<std::string_view, N>& names )
        {
            const auto name = [&]( Method v ) { return names[static_cast<std::size_t>( v )]; };
            if ( !m.Lowered() )
                return std::format( "{}: {}", label, name( m.Effective ) );
            std::string scratch;
            return std::format( "{}: {} -> {} ({})", label, name( m.Requested ), name( m.Effective ),
                                LimitText( m.Limit, scalabilityKey, scratch, name( m.Ceiling ), m.Program ) );
        }

        constexpr std::string_view kGIKey          = "GlobalIllumination.Method";
        constexpr std::string_view kReflectionsKey = "Reflections.Method";
        constexpr std::string_view kShadowsKey     = "Shadows.Method";
    } // namespace

    RenderMethods ResolveRenderMethods( const Core::PostProcessSettings& post, const ResolvedQuality& quality,
                                        const CapabilityCatalog& catalog, bool gBuffer,
                                        const ProgramRefusals& refusals )
    {
        const DeviceOffers device = OffersOf( catalog );
        RenderMethods      out;
        out.GlobalIllumination = ResolveAxis(
             post.GlobalIlluminationMethod, quality.As<GIMethod>( Parameter::GlobalIlluminationMethod ),
             OverriddenIn( quality, Parameter::GlobalIlluminationMethod ), device, gBuffer,
             std::span<const MethodRefusal<GIMethod>>( refusals.GlobalIllumination ) );
        out.Reflections =
             ResolveAxis( post.ReflectionsMethod, quality.As<ReflectionMethod>( Parameter::ReflectionsMethod ),
                          OverriddenIn( quality, Parameter::ReflectionsMethod ), device, gBuffer,
                          std::span<const MethodRefusal<ReflectionMethod>>( refusals.Reflections ) );
        if ( ( out.GlobalIllumination.Effective == GIMethod::PathTraced ) !=
             ( out.Reflections.Effective == ReflectionMethod::PathTraced ) )
        {
            LowerUnpairedPathTraced( out.GlobalIllumination, device, gBuffer,
                                     std::span<const MethodRefusal<GIMethod>>( refusals.GlobalIllumination ) );
            LowerUnpairedPathTraced( out.Reflections, device, gBuffer,
                                     std::span<const MethodRefusal<ReflectionMethod>>( refusals.Reflections ) );
        }
        out.Shadows = ResolveAxis( post.ShadowsMethod, quality.As<ShadowMethod>( Parameter::ShadowsMethod ),
                                   OverriddenIn( quality, Parameter::ShadowsMethod ), device, gBuffer,
                                   std::span<const MethodRefusal<ShadowMethod>>( refusals.Shadows ) );
        // Stochastic's visibility source, down the same order as the methods: the ray query, else what VirtualMaps
        // needs (the virtual map's pages), else the screen trace, which needs nothing but the G-buffer.
        const bool virtualMaps = device.FragmentStores &&
                                 device.RenderArea >= NeedsRenderArea( ShadowMethod::VirtualMaps ) &&
                                 RefusedProgram( std::span<const MethodRefusal<ShadowMethod>>( refusals.Shadows ),
                                                 ShadowMethod::VirtualMaps )
                                      .empty();
        out.RaylessVisibility = virtualMaps ? StochasticVisibility::VirtualMapsAndScreen : StochasticVisibility::Screen;
        out.DeviceVisibility  = device.RayQuery ? StochasticVisibility::RayQuery : out.RaylessVisibility;
        out.Visibility        = out.DeviceVisibility;
        out.ContactShadows       = post.ContactShadows && kContactShadowsImplemented;
        out.DistanceFieldShadows = post.DistanceFieldShadows && kDistanceFieldShadowsImplemented;
        return out;
    }

    void LowerShadowsForFrame( RenderMethods& methods, const bool rayTracedSceneReady )
    {
        methods.Visibility = methods.DeviceVisibility;
        if ( rayTracedSceneReady )
            return;
        if ( methods.Shadows.Effective == ShadowMethod::Stochastic )
        {
            // MegaLights runs without rays: the frame's samples test the rayless source (the overlay row names it).
            methods.Visibility = methods.RaylessVisibility;
            return;
        }
        if ( !NeedsRayQuery( methods.Shadows.Effective ) )
            return;
        methods.Shadows.Effective = ShadowMethod::CascadedMaps;
        if ( methods.Shadows.Limit == MethodLimit::None )
            methods.Shadows.Limit = MethodLimit::FrameScene;
    }

    bool ShadowsTraceRays( const RenderMethods& methods )
    {
        return NeedsRayQuery( methods.Shadows.Effective ) ||
               ( methods.Shadows.Effective == ShadowMethod::Stochastic &&
                 methods.Visibility == StochasticVisibility::RayQuery );
    }

    bool PathTraced( const RenderMethods& methods )
    {
        return methods.GlobalIllumination.Effective == GIMethod::PathTraced &&
               methods.Reflections.Effective == ReflectionMethod::PathTraced;
    }

    void LowerPathTracingForFrame( RenderMethods& methods, const bool rayTracedSceneReady )
    {
        if ( rayTracedSceneReady || !PathTraced( methods ) )
            return;
        methods.GlobalIllumination.Effective = GIMethod::HardwareTraced;
        methods.Reflections.Effective        = ReflectionMethod::HardwareTraced;
        if ( methods.GlobalIllumination.Limit == MethodLimit::None )
            methods.GlobalIllumination.Limit = MethodLimit::FrameScene;
        if ( methods.Reflections.Limit == MethodLimit::None )
            methods.Reflections.Limit = MethodLimit::FrameScene;
    }

    std::vector<std::string> FormatRenderMethods( const RenderMethods& methods )
    {
        return { FormatAxis( "GI", kGIKey, methods.GlobalIllumination, kGIMethodNames ),
                 FormatAxis( "Reflections", kReflectionsKey, methods.Reflections, kReflectionMethodNames ),
                 FormatAxis( "Shadows", kShadowsKey, methods.Shadows, kShadowMethodNames ) +
                      ( methods.Shadows.Effective == ShadowMethod::Stochastic
                             ? std::format( " (visibility {})",
                                            kStochasticVisibilityNames[static_cast<std::size_t>( methods.Visibility )] )
                             : std::string{} ) };
    }

    void ReportRenderMethods( const RenderMethods& methods )
    {
        // Process-wide: several SceneRenderers resolve the same downgrade, and it is still one cause.
        static std::mutex            s_Mutex;
        static std::set<std::string> s_Logged;

        const bool lowered[] = { methods.GlobalIllumination.Lowered(), methods.Reflections.Lowered(),
                                 methods.Shadows.Lowered() };
        if ( !lowered[0] && !lowered[1] && !lowered[2] )
            return;
        const std::vector<std::string> rows = FormatRenderMethods( methods );
        const std::lock_guard          lock( s_Mutex );
        for ( std::size_t i = 0; i < rows.size(); ++i )
            if ( lowered[i] && s_Logged.insert( rows[i] ).second )
                LOG_WARN( "[RenderMethods] {}", rows[i] );
    }
} // namespace Desert::Graphic
