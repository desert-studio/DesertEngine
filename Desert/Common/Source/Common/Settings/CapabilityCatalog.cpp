#include <Common/Settings/CapabilityCatalog.hpp>

#include <format>
#include <iterator>
#include <string_view>
#include <type_traits>

namespace Common::Scalability
{
    namespace
    {
        std::string_view Name( AntiAliasingMethod value )
        {
            switch ( value )
            {
                case AntiAliasingMethod::None:
                    return "None";
                case AntiAliasingMethod::FXAA:
                    return "FXAA";
                case AntiAliasingMethod::SMAA:
                    return "SMAA";
                case AntiAliasingMethod::MSAA:
                    return "MSAA";
                case AntiAliasingMethod::TAA:
                    return "TAA";
                case AntiAliasingMethod::FSRNative:
                    return "FSRNative";
                case AntiAliasingMethod::DLAA:
                    return "DLAA";
            }
            return "?";
        }

        std::string_view Name( Upscaler value )
        {
            switch ( value )
            {
                case Upscaler::None:
                    return "None";
                case Upscaler::TAAU:
                    return "TAAU";
                case Upscaler::FSR:
                    return "FSR";
                case Upscaler::DLSS:
                    return "DLSS";
                case Upscaler::XeSS:
                    return "XeSS";
                case Upscaler::MetalFX:
                    return "MetalFX";
            }
            return "?";
        }

        std::string_view Name( RayTracingMode value )
        {
            switch ( value )
            {
                case RayTracingMode::None:
                    return "None";
                case RayTracingMode::RayQuery:
                    return "RayQuery";
                case RayTracingMode::RayTracingPipeline:
                    return "RayTracingPipeline";
            }
            return "?";
        }

        std::string_view Name( DisplayOutput value )
        {
            switch ( value )
            {
                case DisplayOutput::SDR_sRGB:
                    return "SDR";
                case DisplayOutput::HDR10_PQ:
                    return "HDR10";
                case DisplayOutput::scRGB_Linear:
                    return "scRGB";
            }
            return "?";
        }

        std::string_view Name( PresentMode value )
        {
            switch ( value )
            {
                case PresentMode::Fifo:
                    return "Fifo";
                case PresentMode::FifoRelaxed:
                    return "FifoRelaxed";
                case PresentMode::Mailbox:
                    return "Mailbox";
                case PresentMode::Immediate:
                    return "Immediate";
            }
            return "?";
        }

        std::string_view Name( TextureCompressionFamily value )
        {
            switch ( value )
            {
                case TextureCompressionFamily::BC:
                    return "BC";
                case TextureCompressionFamily::ASTC:
                    return "ASTC";
            }
            return "?";
        }

        std::string_view Name( GpuTiming value )
        {
            switch ( value )
            {
                case GpuTiming::None:
                    return "None";
                case GpuTiming::EncoderBounds:
                    return "EncoderBounds";
                case GpuTiming::AnyStage:
                    return "AnyStage";
            }
            return "?";
        }

        std::string_view Name( DeviceClass value )
        {
            switch ( value )
            {
                case DeviceClass::Unknown:
                    return "Unknown";
                case DeviceClass::Integrated:
                    return "Integrated";
                case DeviceClass::AppleUnified:
                    return "AppleUnified";
                case DeviceClass::Discrete:
                    return "Discrete";
            }
            return "?";
        }

        // "label=a,b,c" — one list of the log line.
        template <typename T>
        void AppendList( std::string& out, std::string_view label, const std::vector<T>& list )
        {
            std::format_to( std::back_inserter( out ), " {}=", label );
            for ( std::size_t i = 0; i < list.size(); ++i )
            {
                if constexpr ( std::is_same_v<T, int> )
                    std::format_to( std::back_inserter( out ), "{}{}", i == 0 ? "" : ",", list[i] );
                else
                    std::format_to( std::back_inserter( out ), "{}{}", i == 0 ? "" : ",", Name( list[i] ) );
            }
        }
    } // namespace

    std::string FormatCatalog( const CapabilityCatalog& catalog )
    {
        std::string out = "[Catalog]";
        AppendList( out, "aa", catalog.AntiAliasingMethods );
        AppendList( out, "msaa", catalog.MSAACounts );
        AppendList( out, "upscale", catalog.Upscalers );
        std::format_to( std::back_inserter( out ), " scale={}..{}%", catalog.RenderScale.MinPercent,
                        catalog.RenderScale.MaxPercent );
        AppendList( out, "rt", catalog.RayTracingModes );
        AppendList( out, "aniso", catalog.AnisotropyLevels );
        AppendList( out, "out", catalog.DisplayOutputs );
        AppendList( out, "present", catalog.PresentModes );
        AppendList( out, "compression", catalog.TextureCompression );
        std::format_to( std::back_inserter( out ), " async={} timing={} class={} vram={}MiB",
                        catalog.AsyncCompute ? "yes" : "no", Name( catalog.Timing ), Name( catalog.Class ),
                        catalog.VideoMemory / ( 1024ull * 1024ull ) );
        return out;
    }
} // namespace Common::Scalability
