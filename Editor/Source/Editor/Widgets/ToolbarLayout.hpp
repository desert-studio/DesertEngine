#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

// The main toolbar's horizontal layout — a pure function, so the rule "playback sits in the centre,
// is one solid group whose size never changes, and never overlaps a neighbour" is tested without ImGui
// (Desert/Tests/Editor/ToolbarLayout).
//
// Why it exists: the playback group used to be placed by hand. Its width was a constant (84 px Play
// plus two 1.6-frame buttons) that matched NO state the bar actually drew — in Edit it drew Play, a
// 0.6-frame chevron and Pause; while playing it dropped the chevron — so the group sat off-centre by a
// different amount in each state, the chevron was narrower than its own frame padding and rendered as
// a clipped sliver detached from Play, and the right group was pinned by a magic 330 px. UE's Level
// Editor toolbar is one segmented group (Play | Options | Pause | Stop) of equal-height slots whose
// slots grey out rather than disappear, which is what makes its position independent of the state.
//
// The group sits on the bar's MIDDLE in every window: when the left groups' labels would push it off the
// middle, those labels collapse to their icons (UE's toolbar entries drop their labels before the bar
// clips; the tooltips still name every button) — ChooseLeftLabels.
namespace Desert::Editor::ToolbarLayout
{
    // Slot widths in multiples of the frame height, so the group scales with the UI font.
    inline constexpr float kPlayWidthFrames    = 3.2f; // the primary action: wide
    inline constexpr float kOptionsWidthFrames = 0.9f; // the ▾ attached to Play
    inline constexpr float kButtonWidthFrames  = 1.6f; // Pause, Next Frame and Stop
    // The seam between two slots of one group: the strip's background shows through it as a divider.
    inline constexpr float kSegmentGap = 1.0f;
    // The least distance between two groups of the bar.
    inline constexpr float kGroupGap = 24.0f;

    enum class PlaybackSlot : std::size_t
    {
        Play,
        Options,
        Pause,
        NextFrame, // UE's Frame Skip: enabled while the world is paused
        Stop,
        Count
    };

    struct Span
    {
        float X     = 0.0f;
        float Width = 0.0f;
    };

    struct PlaybackGroup
    {
        std::array<Span, static_cast<std::size_t>( PlaybackSlot::Count )> Slots{};
        float                                                             Width = 0.0f;

        const Span& operator[]( PlaybackSlot slot ) const
        {
            return Slots[static_cast<std::size_t>( slot )];
        }
    };

    /// The five slots laid out from `x`, each `frameHeight` tall. Every slot is always present — a slot
    /// that does not apply in the current scene state is drawn disabled — so the group's width, the sum
    /// of the slots every state draws, depends on the frame height alone.
    inline PlaybackGroup LayoutPlaybackGroup( const float frameHeight, const float x = 0.0f )
    {
        const std::array<float, static_cast<std::size_t>( PlaybackSlot::Count )> widths = {
             frameHeight * kPlayWidthFrames, frameHeight * kOptionsWidthFrames, frameHeight * kButtonWidthFrames,
             frameHeight * kButtonWidthFrames, frameHeight * kButtonWidthFrames };
        PlaybackGroup group;
        float         cursor = x;
        for ( std::size_t i = 0; i < widths.size(); ++i )
        {
            if ( i > 0 )
                cursor += kSegmentGap;
            group.Slots[i] = Span{ cursor, widths[i] };
            cursor += widths[i];
        }
        group.Width = cursor - x;
        return group;
    }

    struct Row
    {
        float ContentMinX = 0.0f; // the bar's content region, screen space
        float ContentMaxX = 0.0f;
        float LeftEnd     = 0.0f; // where the left groups actually ended this frame
        float CentreWidth = 0.0f; // the playback group
        float RightWidth  = 0.0f; // the right-hand group, measured from its labels
    };

    struct RowPlacement
    {
        float CentreX = 0.0f;
        float RightX  = 0.0f;
    };

    /// The centre group sits on the bar's middle while there is room, and slides only as far as it must
    /// to keep kGroupGap from both neighbours; the right group sits flush right. On a bar too narrow for
    /// all three, the groups keep their order and gaps and the overflow leaves the bar on the right
    /// (clipped), never on top of another group.
    inline RowPlacement PlaceRow( const Row& row )
    {
        const float mid   = ( row.ContentMinX + row.ContentMaxX ) * 0.5f;
        const float lo    = row.LeftEnd + kGroupGap;
        const float hi    = row.ContentMaxX - row.RightWidth - kGroupGap - row.CentreWidth;
        const float ideal = mid - row.CentreWidth * 0.5f;

        RowPlacement placement;
        placement.CentreX = hi < lo ? lo : std::clamp( ideal, lo, hi );
        placement.RightX =
             std::max( row.ContentMaxX - row.RightWidth, placement.CentreX + row.CentreWidth + kGroupGap );
        return placement;
    }

    // A separator between two left groups: a gap, a 1 px line, a gap. The row is PLACED from these numbers and
    // the line is drawn at SeparatorLineX — there is no ImGui cursor arithmetic for it to disagree with (two
    // SameLine calls in a row both measure from the previous ITEM, so the old "8 + 9" advanced 9: 24 px over
    // three separators between the measure and the row the buttons drew).
    inline constexpr float kSeparatorGapBefore = 8.0f;
    inline constexpr float kSeparatorLine      = 1.0f;
    inline constexpr float kSeparatorGapAfter  = 8.0f;
    inline constexpr float kSeparatorAdvance   = kSeparatorGapBefore + kSeparatorLine + kSeparatorGapAfter;

    /// One button of the left groups, measured both ways by the caller (the same text measure the button
    /// draws with). A button whose label is data rather than a name (the snap steps) has Compact ==
    /// Labelled: collapsing it would hide the value it exists to report.
    struct LeftButton
    {
        float Labelled       = 0.0f;
        float Compact        = 0.0f;
        bool  AfterSeparator = false; // a separator precedes it rather than the item spacing
    };

    /// Where each left button's left edge sits, from the row's start: THE placement the toolbar draws at
    /// (it sets the cursor to these) and the one LeftGroupsWidth reads — the measure and the drawn row are
    /// one computation.
    inline std::vector<float> PlaceLeftButtons( const std::span<const LeftButton> buttons, const float itemSpacing,
                                                const bool compact, const float startX )
    {
        std::vector<float> xs;
        xs.reserve( buttons.size() );
        float x = startX;
        for ( std::size_t i = 0; i < buttons.size(); ++i )
        {
            if ( i > 0 )
                x += ( compact ? buttons[i - 1].Compact : buttons[i - 1].Labelled ) +
                     ( buttons[i].AfterSeparator ? kSeparatorAdvance : itemSpacing );
            xs.push_back( x );
        }
        return xs;
    }

    /// The separator line before a button placed at @p buttonX (one with AfterSeparator).
    inline float SeparatorLineX( const float buttonX )
    {
        return buttonX - kSeparatorGapAfter - kSeparatorLine;
    }

    /// The width the left groups occupy, from the first button's left edge to the last one's right.
    inline float LeftGroupsWidth( const std::span<const LeftButton> buttons, const float itemSpacing,
                                  const bool compact )
    {
        if ( buttons.empty() )
            return 0.0f;
        const std::vector<float> xs = PlaceLeftButtons( buttons, itemSpacing, compact, 0.0f );
        return xs.back() + ( compact ? buttons.back().Compact : buttons.back().Labelled );
    }

    enum class LeftLabels
    {
        Shown,
        IconsOnly,
    };

    /// Labels are shown while, WITH them, the centre group still sits on the bar's exact middle; otherwise
    /// they collapse, so the centre group moves only when even the icons alone would sit under it. `row`
    /// carries the LABELLED left end.
    inline LeftLabels ChooseLeftLabels( const Row& labelled )
    {
        const float ideal = ( labelled.ContentMinX + labelled.ContentMaxX ) * 0.5f - labelled.CentreWidth * 0.5f;
        return std::abs( PlaceRow( labelled ).CentreX - ideal ) <= 0.5f ? LeftLabels::Shown
                                                                        : LeftLabels::IconsOnly;
    }
} // namespace Desert::Editor::ToolbarLayout
