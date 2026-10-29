#include <Common/Settings/DisplaySettings.hpp>

#include <format>

namespace Common::Scalability
{
    ResolvedPresentMode ResolvePresentMode( const DisplaySettings& settings, const CapabilityCatalog& catalog )
    {
        if ( settings.VSync )
            return { PresentMode::Fifo, {} };
        if ( CapabilityCatalog::Offers( catalog.PresentModes, PresentMode::Immediate ) )
            return { PresentMode::Immediate, {} };
        if ( CapabilityCatalog::Offers( catalog.PresentModes, PresentMode::Mailbox ) )
            return { PresentMode::Mailbox, {} };
        return { PresentMode::Fifo, "the surface offers no present mode without VSync" };
    }

    std::string_view DisplayOutputName( const DisplayOutput output )
    {
        switch ( output )
        {
            case DisplayOutput::SDR_sRGB:
                return "SDR (sRGB)";
            case DisplayOutput::HDR10_PQ:
                return "HDR10 (Rec.2020 PQ)";
            case DisplayOutput::scRGB_Linear:
                return "scRGB linear (EDR)";
        }
        return "unknown";
    }

    ResolvedDisplayOutput ResolveDisplayOutput( const DisplaySettings& settings, const CapabilityCatalog& catalog )
    {
        if ( !CapabilityCatalog::Offers( catalog.DisplayOutputs, settings.Output ) )
            return { settings.Output, std::format( "Display.Output {} is not in this display's DisplayOutputs "
                                                   "(the surface does not list its format + colour space)",
                                                   DisplayOutputName( settings.Output ) ) };
        if ( settings.Output == DisplayOutput::HDR10_PQ )
            return { settings.Output, "Display.Output HDR10 (Rec.2020 PQ) has no output transform yet "
                                      "(HDR-OUTPUT p2); choose SDR or scRGB" };
        if ( settings.Output != DisplayOutput::SDR_sRGB && !( settings.PaperWhiteNits > 0.0f ) )
            return { settings.Output, "Display.PaperWhiteNits must be > 0 for an HDR output" };
        return { settings.Output, {} };
    }
} // namespace Common::Scalability
