#include "RenderMethods.hpp"

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <format>
#include <mutex>
#include <set>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        using namespace Common::Scalability;

        // What the device runs of ray tracing: inline ray query (a pipeline device offers it too) and the ray
        // tracing pipeline.
        struct DeviceOffers
        {
            bool RayQuery = false;
            bool Pipeline = false;
        };

        [[nodiscard]] DeviceOffers OffersOf( const CapabilityCatalog& catalog )
        {
            return { .RayQuery = std::any_of( catalog.RayTracingModes.begin(), catalog.RayTracingModes.end(),
                                              []( RayTracingMode m ) { return m != RayTracingMode::None; } ),
                     .Pipeline = CapabilityCatalog::Offers( catalog.RayTracingModes,
                                                            RayTracingMode::RayTracingPipeline ) };
        }

        // Down the ladder from r.Effective past every method the device cannot run, the render path cannot feed
        // or this build does not implement; the first limit met is kept. The front (Off / CascadedMaps) needs
        // nothing, so the walk always ends.
        template <typename Method>
        void WalkDown( ResolvedMethod<Method>& r, const DeviceOffers device, const bool gBuffer )
        {
            for ( ;; )
            {
                MethodLimit blocked = MethodLimit::None;
                if ( NeedsRayQuery( r.Effective ) && !device.RayQuery )
                    blocked = MethodLimit::Device;
                else if ( NeedsRayTracingPipeline( r.Effective ) && !device.Pipeline )
                    blocked = MethodLimit::DevicePipeline;
                else if ( NeedsGBuffer( r.Effective ) && !gBuffer )
                    blocked = MethodLimit::RenderPath;
                else if ( !IsImplemented( r.Effective ) )
                    blocked = MethodLimit::NotImplemented;
                if ( blocked == MethodLimit::None )
                    return;
                if ( r.Limit == MethodLimit::None )
                    r.Limit = blocked;
                r.Effective = static_cast<Method>( static_cast<int>( r.Effective ) - 1 );
            }
        }

        // The one rule, per axis: min( request, ceiling ), then down the ladder (the enum's order) past every
        // method the device cannot run, the render path cannot feed or this build does not implement. The
        // ladder's front (Off / CascadedMaps) is implemented and needs nothing, so the walk always ends.
        template <typename Method>
        ResolvedMethod<Method> ResolveAxis( Method requested, Method ceiling, const DeviceOffers device,
                                            const bool gBuffer )
        {
            ResolvedMethod<Method> r{ .Requested = requested, .Ceiling = ceiling, .Effective = requested };
            if ( ceiling < requested )
            {
                r.Effective = ceiling;
                r.Limit     = MethodLimit::Scalability;
            }
            WalkDown( r, device, gBuffer );
            return r;
        }

        // The path tracer is ONE integrator for GI and reflections: an axis that resolved to PathTraced while the
        // other did not is lowered one step and walked on, naming the pair as its limit.
        template <typename Method>
        void LowerUnpairedPathTraced( ResolvedMethod<Method>& r, const DeviceOffers device, const bool gBuffer )
        {
            if ( r.Effective != Method::PathTraced )
                return;
            r.Effective = static_cast<Method>( static_cast<int>( Method::PathTraced ) - 1 );
            if ( r.Limit == MethodLimit::None )
                r.Limit = MethodLimit::PathTracerPair;
            WalkDown( r, device, gBuffer );
        }

        std::string_view LimitText( MethodLimit limit, std::string_view scalabilityKey, std::string& scratch,
                                    std::string_view ceiling )
        {
            switch ( limit )
            {
                case MethodLimit::None:
                    return {};
                case MethodLimit::Scalability:
                    scratch = std::format( "Scalability {} is {}", scalabilityKey, ceiling );
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
                                LimitText( m.Limit, scalabilityKey, scratch, name( m.Ceiling ) ) );
        }

        constexpr std::string_view kGIKey          = "GlobalIllumination.Method";
        constexpr std::string_view kReflectionsKey = "Reflections.Method";
        constexpr std::string_view kShadowsKey     = "Shadows.Method";
    } // namespace

    RenderMethods ResolveRenderMethods( const Core::PostProcessSettings& post, const ResolvedQuality& quality,
                                        const CapabilityCatalog& catalog, bool gBuffer )
    {
        const DeviceOffers device = OffersOf( catalog );
        RenderMethods      out;
        out.GlobalIllumination =
             ResolveAxis( post.GlobalIlluminationMethod,
                          quality.As<GIMethod>( Parameter::GlobalIlluminationMethod ), device, gBuffer );
        out.Reflections = ResolveAxis( post.ReflectionsMethod,
                                       quality.As<ReflectionMethod>( Parameter::ReflectionsMethod ), device,
                                       gBuffer );
        if ( ( out.GlobalIllumination.Effective == GIMethod::PathTraced ) !=
             ( out.Reflections.Effective == ReflectionMethod::PathTraced ) )
        {
            LowerUnpairedPathTraced( out.GlobalIllumination, device, gBuffer );
            LowerUnpairedPathTraced( out.Reflections, device, gBuffer );
        }
        out.Shadows = ResolveAxis( post.ShadowsMethod, quality.As<ShadowMethod>( Parameter::ShadowsMethod ),
                                   device, gBuffer );
        out.ContactShadows       = post.ContactShadows && kContactShadowsImplemented;
        out.DistanceFieldShadows = post.DistanceFieldShadows && kDistanceFieldShadowsImplemented;
        return out;
    }

    void LowerShadowsForFrame( RenderMethods& methods, const bool rayTracedSceneReady )
    {
        if ( rayTracedSceneReady || methods.Shadows.Effective != ShadowMethod::HardwareTraced )
            return;
        methods.Shadows.Effective = ShadowMethod::CascadedMaps;
        if ( methods.Shadows.Limit == MethodLimit::None )
            methods.Shadows.Limit = MethodLimit::FrameScene;
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
                 FormatAxis( "Shadows", kShadowsKey, methods.Shadows, kShadowMethodNames ) };
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
