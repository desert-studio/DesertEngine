#include <Common/Settings/DisplaySettings.hpp>

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
} // namespace Common::Scalability
