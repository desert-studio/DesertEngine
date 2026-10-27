#include <Engine/Graphic/View/ViewFrame.hpp>

#include <Engine/Graphic/API/UpscalerBuild.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        namespace Scal = Common::Scalability;

        // UE r.TemporalAASamples at native scale; TAAU multiplies it by the output/render pixel ratio.
        constexpr uint32_t kJitterSamplesPerOutputPixel = 8;
        // UE caps the TAAU sequence: beyond 64 positions the sequence is longer than the history's effective
        // memory (the blend forgets a sample before the sequence comes back to it).
        constexpr uint32_t kMaxJitterSequenceLength = 64;

        [[nodiscard]] std::string_view MethodName( const Scal::AntiAliasingMethod method )
        {
            const auto index = static_cast<std::size_t>( method );
            return index < Scal::kAntiAliasingMethodNames.size() ? Scal::kAntiAliasingMethodNames[index]
                                                                 : std::string_view( "<unknown AntiAliasingMethod>" );
        }

        // round-to-nearest( side * percent / 100 ), integer arithmetic so 1080 x 67 % is 724 on every compiler.
        [[nodiscard]] uint64_t ScaleSide( const uint32_t side, const int percent )
        {
            return ( static_cast<uint64_t>( side ) * static_cast<uint64_t>( percent ) + 50u ) / 100u;
        }

        // Radical inverse of @p index (>= 1) in @p base: the Halton sequence's value at that index, in (0, 1).
        [[nodiscard]] double Halton( uint32_t index, const uint32_t base )
        {
            double result   = 0.0;
            double fraction = 1.0 / static_cast<double>( base );
            while ( index > 0 )
            {
                result += fraction * static_cast<double>( index % base );
                index /= base;
                fraction /= static_cast<double>( base );
            }
            return result;
        }
    } // namespace

    Common::ResultStr<ResolutionSplit> MakeResolutionSplit( const ViewExtent output, const int renderScalePercent )
    {
        if ( renderScalePercent <= 0 )
            return Common::MakeFormattedError<ResolutionSplit>(
                 "MakeResolutionSplit: render scale {} % of {}x{} is not a scale (must be > 0)",
                 renderScalePercent, output.Width, output.Height );

        ResolutionSplit split;
        split.Output             = output;
        split.RenderScalePercent = renderScalePercent;
        split.Mode               = Scal::ScaleMode::Native;
        if ( renderScalePercent < 100 )
            split.Mode = Scal::ScaleMode::Upscale;
        else if ( renderScalePercent > 100 )
            split.Mode = Scal::ScaleMode::Supersample;

        if ( output.Width == 0 || output.Height == 0 )
            return Common::MakeSuccess( split ); // Render stays empty: a minimised view draws nothing

        const uint64_t width  = std::max<uint64_t>( 1u, ScaleSide( output.Width, renderScalePercent ) );
        const uint64_t height = std::max<uint64_t>( 1u, ScaleSide( output.Height, renderScalePercent ) );
        if ( width > kMaxViewExtentSide || height > kMaxViewExtentSide )
            return Common::MakeFormattedError<ResolutionSplit>(
                 "MakeResolutionSplit: {} % of the {}x{} output renders at {}x{}, above the largest view side {}",
                 renderScalePercent, output.Width, output.Height, width, height, kMaxViewExtentSide );

        split.Render = ViewExtent{ static_cast<uint32_t>( width ), static_cast<uint32_t>( height ) };
        return Common::MakeSuccess( split );
    }

    Common::ResultStr<TemporalMethod> SelectTemporalMethod( const Scal::PathAntiAliasing& path,
                                                            const ResolutionSplit&        split )
    {
        switch ( path.Method )
        {
            // No history: nothing temporal at any split (below 100 % the spatial upscale, UpscalerForScale
            // Spatial, brings the frame to the output and is not temporal).
            case Scal::AntiAliasingMethod::None:
            case Scal::AntiAliasingMethod::FXAA:
            case Scal::AntiAliasingMethod::SMAA:
            case Scal::AntiAliasingMethod::MSAA:
                return Common::MakeSuccess( TemporalMethod::None );
            case Scal::AntiAliasingMethod::TAA:
                return Common::MakeSuccess( split.Mode == Scal::ScaleMode::Upscale ? TemporalMethod::TAAU
                                                                                   : TemporalMethod::TAA );
            // One method at every split <= 100 %. Above it (an editor viewport's Screen Percentage > 100 under a
            // game setting Resolve kept at <= 100 %) the method is selected too, and its Supports refuses the
            // supersample split, so ResolveViewResolution clamps the view to 100 % and says why.
            case Scal::AntiAliasingMethod::TSR:
                return Common::MakeSuccess( TemporalMethod::TSR );
            case Scal::AntiAliasingMethod::DLSS:
                if ( kDlssBuilt )
                    return Common::MakeSuccess( TemporalMethod::DLSS );
                break;
            case Scal::AntiAliasingMethod::DLSSRayReconstruction:
                if ( kDlssBuilt )
                    return Common::MakeSuccess( TemporalMethod::DLSSRR );
                break;
            case Scal::AntiAliasingMethod::FSR:
                if ( kFsrBuilt )
                    return Common::MakeSuccess( TemporalMethod::FSR );
                break;
            case Scal::AntiAliasingMethod::XeSS:
            case Scal::AntiAliasingMethod::MetalFX:
                break;
        }
        return Common::MakeFormattedError<TemporalMethod>(
             "SelectTemporalMethod: anti-aliasing method {} ({}) at render scale {} % has no implementation in "
             "this build (Scalability Resolve keeps it only where the device catalog offers it)",
             MethodName( path.Method ), static_cast<int>( path.Method ), split.RenderScalePercent );
    }

    uint32_t TemporalJitterSequenceLength( const TemporalMethod method, const ResolutionSplit& split )
    {
        switch ( method )
        {
            case TemporalMethod::None:
                return 0;
            case TemporalMethod::TAA:
                return kJitterSamplesPerOutputPixel;
            // DLSS: NVIDIA's rule is UE's TAAU rule (8 phases per output pixel, the render/output pixel ratio
            // times it), so it shares TAAU's sequence length below. FSR has its own phase count
            // (Fsr3Upscaler::JitterSequenceLength overrides the ITemporalUpscaler default); it is listed so this
            // free function stays total. TSR is UE's TAAU rule too (TemporalSuperResolution.cpp: the jitter
            // sequence of a temporal upsample, 8 samples per output pixel).
            case TemporalMethod::TAAU:
            case TemporalMethod::TSR:
            case TemporalMethod::DLSS:
            case TemporalMethod::FSR:
                break;
        }

        const uint64_t renderPixels = static_cast<uint64_t>( split.Render.Width ) * split.Render.Height;
        const uint64_t outputPixels = static_cast<uint64_t>( split.Output.Width ) * split.Output.Height;
        if ( renderPixels == 0 )
            return kJitterSamplesPerOutputPixel; // an empty view draws nothing; any length is consistent

        // ceil( 8 * output / render ), never below the native length, capped.
        const uint64_t length = ( kJitterSamplesPerOutputPixel * outputPixels + renderPixels - 1 ) / renderPixels;
        return static_cast<uint32_t>(
             std::clamp<uint64_t>( length, kJitterSamplesPerOutputPixel, kMaxJitterSequenceLength ) );
    }

    glm::vec2 TemporalJitterPixels( const uint32_t index, const uint32_t length )
    {
        if ( length == 0 )
            return glm::vec2( 0.0f );
        const uint32_t haltonIndex = index % length + 1; // Halton(0) is 0 in every base: start at 1
        return { static_cast<float>( Halton( haltonIndex, 2 ) - 0.5 ),
                 static_cast<float>( Halton( haltonIndex, 3 ) - 0.5 ) };
    }

    glm::vec2 JitterPixelsToNdc( const glm::vec2 pixels, const ViewExtent render )
    {
        if ( render.Width == 0 || render.Height == 0 )
            return glm::vec2( 0.0f );
        return 2.0f * pixels /
               glm::vec2( static_cast<float>( render.Width ), static_cast<float>( render.Height ) );
    }

    glm::mat4 ApplyJitter( const glm::mat4& projection, const glm::vec2 ndc )
    {
        return glm::translate( glm::mat4( 1.0f ), glm::vec3( ndc.x, ndc.y, 0.0f ) ) * projection;
    }

    ViewFrame MakeStillViewFrame( const glm::mat4& view, const glm::mat4& projection,
                                  const glm::vec3& cameraPosition, const double timeSeconds )
    {
        ViewFrame f;
        f.View                      = view;
        f.InvView                   = glm::inverse( view );
        f.Projection                = projection;
        f.InvProjection             = glm::inverse( projection );
        f.JitteredProjection        = projection;
        f.ViewProjection            = projection * view;
        f.InvViewProjection         = glm::inverse( f.ViewProjection );
        f.JitteredViewProjection    = f.ViewProjection;
        f.InvJitteredViewProjection = f.InvViewProjection;
        f.CameraPosition            = cameraPosition;

        f.PrevView                   = f.View;
        f.PrevProjection             = f.Projection;
        f.PrevViewProjection         = f.ViewProjection;
        f.PrevInvViewProjection      = f.InvViewProjection;
        f.PrevJitteredViewProjection = f.JitteredViewProjection;
        f.PrevCameraPosition         = f.CameraPosition;

        f.TimeSeconds     = timeSeconds;
        f.PrevTimeSeconds = timeSeconds;
        f.PreExposure     = 1.0f; // a still camera has no eye adaptation: its output is stored absolute
        f.PrevPreExposure = f.PreExposure;
        f.HistoryReset    = HistoryResetReason::FirstFrame;
        return f;
    }
} // namespace Desert::Graphic
