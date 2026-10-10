#include "TrackFilter.hpp"

#include <algorithm>

namespace Desert::Editor::Sequencer
{
    bool TrackPasses( const Animation::Timeline::Track& track, const bool bindingSelected,
                      const TrackFilters& filters )
    {
        if ( filters.Selected && !bindingSelected )
            return false;
        return !filters.Keyed || Animation::Timeline::TrackHasKeys( track );
    }

    bool BindingPasses( const Animation::Timeline::Sequence& sequence, const Animation::Timeline::Binding& binding,
                        const bool bindingSelected, const TrackFilters& filters )
    {
        if ( filters.Selected && !bindingSelected )
            return false;
        if ( !filters.Keyed )
            return true;
        return std::ranges::any_of( sequence.Tracks, [&]( const Animation::Timeline::Track& track ) {
            return track.Binding == binding.Guid && TrackPasses( track, bindingSelected, filters );
        } );
    }
} // namespace Desert::Editor::Sequencer
