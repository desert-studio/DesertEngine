#include <Engine/Graphic/DisplayOutput.hpp>

#include <format>

namespace Desert::Graphic
{
    namespace
    {
        DisplayOutputState s_Current;
    }

    const DisplayOutputState& CurrentDisplayOutput()
    {
        return s_Current;
    }

    void PublishDisplayOutput( const DisplayOutputState& state )
    {
        s_Current = state;
    }

    Common::ResultStr<DisplayOutputState>
    ResolveDisplayOutputState( const Common::Scalability::DisplayOutput     output,
                               const Common::Scalability::DisplaySettings& settings, const DisplayLuminance& os )
    {
        DisplayOutputState state;
        state.Output = output;
        if ( output == Common::Scalability::DisplayOutput::SDR_sRGB )
            return Common::MakeSuccess( state );

        state.PaperWhiteNits = settings.PaperWhiteNits > 0.0f ? settings.PaperWhiteNits : os.PaperWhiteNits;
        if ( !( state.PaperWhiteNits > 0.0f ) )
            return Common::MakeError<DisplayOutputState>(
                 std::format( "no paper white: Display.PaperWhiteNits is {} and {} reports none",
                              settings.PaperWhiteNits, os.Source ) );
        if ( settings.PeakNits > 0.0f )
            state.PeakNits = settings.PeakNits;
        else if ( os.PeakNits > 0.0f )
            state.PeakNits = os.PeakNits;
        else
        {
            // macOS answers maximumExtendedDynamicRangeColorComponentValue = 1.0 until a layer on the screen asks
            // for EDR (CAMetalLayer wantsExtendedDynamicRangeContent, which the scRGB swapchain itself sets): the
            // "now" headroom cannot decide whether to build the EDR swapchain. The display's capability is the
            // potential headroom (Apple, "Displaying HDR content in a Metal layer"); a reference preset (XDR
            // reference modes) pins it. Once EDR is on, the current headroom is the truth.
            const float headroom = os.EdrHeadroom > 1.0f      ? os.EdrHeadroom
                                   : os.ReferenceHeadroom > 1.0f ? os.ReferenceHeadroom
                                                                 : os.PotentialHeadroom;
            state.PeakNits = headroom * state.PaperWhiteNits;
        }
        state.Headroom = state.PeakNits / state.PaperWhiteNits;
        if ( !( state.Headroom > 1.0f ) )
            return Common::MakeError<DisplayOutputState>( std::format(
                 "the display has no headroom above paper white (peak {:.0f} nits, paper white {:.0f} nits; {} "
                 "EDR headroom {:.3f}, potential {:.3f}, reference {:.3f}): an HDR output would present SDR",
                 state.PeakNits, state.PaperWhiteNits, os.Source, os.EdrHeadroom, os.PotentialHeadroom,
                 os.ReferenceHeadroom ) );
        return Common::MakeSuccess( state );
    }

#if !defined( __APPLE__ )
    // Windows: DXGI IDXGIOutput6::GetDesc1 on the output the window covers (HDR-OUTPUT p2; no HDR monitor on the
    // Windows machine today). Until then an HDR output is refused here, by name.
    Common::ResultStr<DisplayLuminance> QueryDisplayLuminance( void* )
    {
        return Common::MakeError<DisplayLuminance>(
             "QueryDisplayLuminance: DXGI GetDesc1 is not implemented yet (HDR-OUTPUT p2)" );
    }
#endif
} // namespace Desert::Graphic
