#pragma once
/**
 * THE SEQUENCER'S TRACK FILTERS — WHICH ROWS OF A LEVEL SEQUENCE THE TIMELINE SHOWS. No ImGui here, so the
 * rule is testable without a window (Desert/Tests/Editor/SequencerTrackFilter).
 *
 * Ported from UE's Sequencer (SequencerTrackFilterCommands.h: ToggleFilter_Selected / ToggleFilter_Keyed):
 *
 *   * SELECTED (SequencerTrackFilter_Selected.cpp) is HIERARCHY-BASED: an actor's binding passes when its entity
 *     is selected in the level, and then every track under it is shown — the filter is about which actor, not
 *     which property. A track of the sequence itself (Camera Cut, the master Event track) is bound to no actor,
 *     so it never passes.
 *   * KEYED (SequencerTrackFilter_Keyed.cpp) is per TRACK: a track passes when any of its sections holds a key
 *     (`Animation::Timeline::TrackHasKeys`); a binding is shown when any of its tracks passes, with only those
 * tracks.
 *
 * Active filters combine with AND, as UE's do: a row is shown only when it passes every one that is on. The
 * on/off state is the user's (EditorPreferences::SequencerFilterSelected / SequencerFilterKeyed), not the
 * sequence's: two people opening one sequence legitimately want different rows.
 */
#include <Engine/Animation/Timeline/Sequence.hpp>

namespace Desert::Editor::Sequencer
{
    struct TrackFilters
    {
        bool Selected = false; ///< show only actors selected in the level
        bool Keyed    = false; ///< show only tracks that hold a key
    };

    /// A track of @p binding's row under @p filters; @p bindingSelected is whether its actor is selected.
    [[nodiscard]] bool TrackPasses( const Animation::Timeline::Track& track, bool bindingSelected,
                                    const TrackFilters& filters );

    /// The actor row of @p binding: shown when it passes Selected and, under Keyed, when any of its tracks passes.
    [[nodiscard]] bool BindingPasses( const Animation::Timeline::Sequence& sequence,
                                      const Animation::Timeline::Binding& binding, bool bindingSelected,
                                      const TrackFilters& filters );
} // namespace Desert::Editor::Sequencer
