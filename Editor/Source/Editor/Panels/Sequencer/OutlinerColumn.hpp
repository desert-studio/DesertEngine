#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace Desert::Editor::Sequencer
{
    /// The Sequencer's Outliner, the column left of the lanes (UE: SSequencerTreeView beside the track area).
    /// It has two parts: the NAMES column, whose width is the panel's state and is dragged by the splitter at
    /// its right edge, and the CONTROLS column after it (a row's + Track / + Key buttons, its value field), of
    /// a fixed width so a row's buttons stay where the hand expects them whatever the names are.
    struct OutlinerColumn
    {
        static constexpr float kDefaultNameWidth = 160.0f;
        static constexpr float kMinNameWidth     = 80.0f;
        static constexpr float kControlsWidth    = 220.0f;
        /// The lane area a dragged splitter leaves at least, so the timeline never collapses under the names.
        static constexpr float kMinLaneWidth = 120.0f;
        /// The gap between a name and the column edge (the splitter's grip lives in it).
        static constexpr float kLabelPadding = 6.0f;
    };

    /// The names column width @p wanted can take in a panel @p panelWidth wide: no narrower than
    /// `kMinNameWidth`, no wider than what leaves the controls column and `kMinLaneWidth` of lanes. A panel too
    /// narrow for even that gets `kMinNameWidth` (the lanes are what shrinks, as everywhere in the timeline).
    [[nodiscard]] float ClampNameColumnWidth( float wanted, float panelWidth );

    /// The width @p text draws at; the panel hands the font's measure, a test a fixed advance.
    using TextMeasure = std::function<float( std::string_view )>;

    struct FittedLabel
    {
        std::string Text;
        bool        Truncated = false; ///< the row shows the full @p text in its tooltip
    };

    inline constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // U+2026

    /// A row's name fitted to @p maxWidth (UE: a long Outliner label ends in "…", the tooltip holds it whole):
    /// the text itself when it fits, else its longest prefix — cut between UTF-8 code points, trailing spaces
    /// dropped — followed by "…". When not even "…" fits, the label is "…" alone, still marked truncated.
    [[nodiscard]] FittedLabel FitLabel( std::string_view text, float maxWidth, const TextMeasure& measure );
} // namespace Desert::Editor::Sequencer
