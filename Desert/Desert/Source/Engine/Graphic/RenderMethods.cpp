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

        [[nodiscard]] bool OffersRayQuery( const CapabilityCatalog& catalog )
        {
            return std::any_of( catalog.RayTracingModes.begin(), catalog.RayTracingModes.end(),
                                []( RayTracingMode m ) { return m != RayTracingMode::None; } );
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

        // The one rule, per axis: min( request, ceiling ), then down the ladder (the enum's order) past every
        // method the device cannot run, the render path cannot feed, this build does not implement or a renderer
        // could not build a program for. The ladder's front (Off / CascadedMaps) is implemented, needs nothing and
        // is never refused (its programs failing is the renderer's initialisation error), so the walk always ends.
        template <typename Method>
        ResolvedMethod<Method> ResolveAxis( Method requested, Method ceiling, bool rayQuery, bool fragmentStores,
                                            uint32_t renderArea, bool gBuffer,
                                            std::span<const MethodRefusal<Method>> refusals )
        {
            ResolvedMethod<Method> r{ .Requested = requested, .Ceiling = ceiling, .Effective = requested };
            if ( ceiling < requested )
            {
                r.Effective = ceiling;
                r.Limit     = MethodLimit::Scalability;
            }
            for ( ;; )
            {
                MethodLimit blocked = MethodLimit::None;
                if ( NeedsRayQuery( r.Effective ) && !rayQuery )
                    blocked = MethodLimit::Device;
                else if ( NeedsFragmentStores( r.Effective ) && !fragmentStores )
                    blocked = MethodLimit::FragmentStores;
                else if ( NeedsRenderArea( r.Effective ) > renderArea )
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
                    break;
                if ( r.Limit == MethodLimit::None )
                    r.Limit = blocked;
                r.Effective = static_cast<Method>( static_cast<int>( r.Effective ) - 1 );
            }
            return r;
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
                case MethodLimit::Device:
                    return "the device offers no ray query";
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
        const bool    rayQuery = OffersRayQuery( catalog );
        RenderMethods out;
        out.GlobalIllumination = ResolveAxis( post.GlobalIlluminationMethod,
                                              quality.As<GIMethod>( Parameter::GlobalIlluminationMethod ), rayQuery,
                                              catalog.FragmentStores, catalog.MaxRenderArea, gBuffer,
                                              std::span<const MethodRefusal<GIMethod>>{} );
        out.Reflections = ResolveAxis( post.ReflectionsMethod,
                                       quality.As<ReflectionMethod>( Parameter::ReflectionsMethod ), rayQuery,
                                       catalog.FragmentStores, catalog.MaxRenderArea, gBuffer,
                                       std::span<const MethodRefusal<ReflectionMethod>>{} );
        out.Shadows = ResolveAxis( post.ShadowsMethod, quality.As<ShadowMethod>( Parameter::ShadowsMethod ), rayQuery,
                                   catalog.FragmentStores, catalog.MaxRenderArea, gBuffer,
                                   std::span<const MethodRefusal<ShadowMethod>>( refusals.Shadows ) );
        out.ContactShadows       = post.ContactShadows && kContactShadowsImplemented;
        out.DistanceFieldShadows = post.DistanceFieldShadows && kDistanceFieldShadowsImplemented;
        return out;
    }

    void LowerShadowsForFrame( RenderMethods& methods, const bool rayTracedSceneReady )
    {
        if ( rayTracedSceneReady || !NeedsRayQuery( methods.Shadows.Effective ) )
            return;
        methods.Shadows.Effective = ShadowMethod::CascadedMaps;
        if ( methods.Shadows.Limit == MethodLimit::None )
            methods.Shadows.Limit = MethodLimit::FrameScene;
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
