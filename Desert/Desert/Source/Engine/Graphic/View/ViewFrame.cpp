#include <Engine/Graphic/View/ViewFrame.hpp>

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

        [[nodiscard]] std::string_view UpscalerName( const Scal::Upscaler upscaler )
        {
            switch ( upscaler )
            {
                case Scal::Upscaler::None:
                    return "None";
                case Scal::Upscaler::TAAU:
                    return "TAAU";
                case Scal::Upscaler::FSR:
                    return "FSR";
                case Scal::Upscaler::DLSS:
                    return "DLSS";
                case Scal::Upscaler::XeSS:
                    return "XeSS";
                case Scal::Upscaler::MetalFX:
                    return "MetalFX";
            }
            return "<unknown Upscaler>";
        }

        [[nodiscard]] std::string_view MethodName( const Scal::AntiAliasingMethod method )
        {
            switch ( method )
            {
                case Scal::AntiAliasingMethod::None:
                    return "None";
                case Scal::AntiAliasingMethod::FXAA:
                    return "FXAA";
                case Scal::AntiAliasingMethod::SMAA:
                    return "SMAA";
                case Scal::AntiAliasingMethod::MSAA:
                    return "MSAA";
                case Scal::AntiAliasingMethod::TAA:
                    return "TAA";
                case Scal::AntiAliasingMethod::FSRNative:
                    return "FSRNative";
                case Scal::AntiAliasingMethod::DLAA:
                    return "DLAA";
            }
            return "<unknown AntiAliasingMethod>";
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
                                                            const ResolutionSplit&        split,
                                                            const Scal::Upscaler          upscaler )
    {
        if ( split.Mode == Scal::ScaleMode::Upscale )
        {
            if ( upscaler == Scal::Upscaler::TAAU )
                return Common::MakeSuccess( TemporalMethod::TAAU );
            if ( upscaler == Scal::Upscaler::None )
                return Common::MakeFormattedError<TemporalMethod>(
                     "SelectTemporalMethod: render scale {} % with Upscaler None (Scalability Resolve never "
                     "produces it: below 100 % an upscaler is required)",
                     split.RenderScalePercent );
            return Common::MakeFormattedError<TemporalMethod>(
                 "SelectTemporalMethod: Upscaler {} at {} % has no implementation in this build",
                 UpscalerName( upscaler ), split.RenderScalePercent );
        }

        if ( upscaler != Scal::Upscaler::None )
            return Common::MakeFormattedError<TemporalMethod>(
                 "SelectTemporalMethod: Upscaler {} at render scale {} % (Scalability Resolve never produces it: "
                 "the upscaler is None at native and supersampled scale)",
                 UpscalerName( upscaler ), split.RenderScalePercent );

        switch ( path.Method )
        {
            case Scal::AntiAliasingMethod::TAA:
                return Common::MakeSuccess( TemporalMethod::TAA );
            case Scal::AntiAliasingMethod::None:
            case Scal::AntiAliasingMethod::FXAA:
            case Scal::AntiAliasingMethod::SMAA:
            case Scal::AntiAliasingMethod::MSAA:
                return Common::MakeSuccess( TemporalMethod::None );
            case Scal::AntiAliasingMethod::FSRNative:
            case Scal::AntiAliasingMethod::DLAA:
                break;
        }
        return Common::MakeFormattedError<TemporalMethod>(
             "SelectTemporalMethod: anti-aliasing method {} ({}) at render scale {} % has no implementation in "
             "this "
             "build",
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
            case TemporalMethod::TAAU:
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
        f.HistoryReset    = HistoryResetReason::FirstFrame;
        return f;
    }
} // namespace Desert::Graphic
