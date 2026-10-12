#include "OutlinerColumn.hpp"

#include <algorithm>
#include <vector>

namespace Desert::Editor::Sequencer
{
    float ClampNameColumnWidth( const float wanted, const float panelWidth )
    {
        const float widest = panelWidth - OutlinerColumn::kControlsWidth - OutlinerColumn::kMinLaneWidth;
        return std::max( OutlinerColumn::kMinNameWidth, std::min( wanted, widest ) );
    }

    namespace
    {
        std::string WithEllipsis( std::string_view prefix )
        {
            while ( !prefix.empty() && prefix.back() == ' ' )
                prefix.remove_suffix( 1 );
            std::string out( prefix );
            out += kEllipsis;
            return out;
        }
    } // namespace

    FittedLabel FitLabel( const std::string_view text, const float maxWidth, const TextMeasure& measure )
    {
        if ( measure( text ) <= maxWidth )
            return { std::string( text ), false };

        // Every place a prefix may end: before a byte that does not continue a UTF-8 sequence.
        std::vector<size_t> cuts;
        for ( size_t i = 0; i < text.size(); ++i )
            if ( ( static_cast<unsigned char>( text[i] ) & 0xC0u ) != 0x80u )
                cuts.push_back( i );

        // The longest prefix whose "prefix…" fits; the width grows with the prefix, so a bisection finds it.
        size_t lo = 0; // cuts[0] == 0: the empty prefix, "…" alone
        size_t hi = cuts.size();
        while ( hi - lo > 1 )
        {
            const size_t mid = ( lo + hi ) / 2;
            if ( measure( WithEllipsis( text.substr( 0, cuts[mid] ) ) ) <= maxWidth )
                lo = mid;
            else
                hi = mid;
        }
        return { WithEllipsis( text.substr( 0, cuts.empty() ? 0 : cuts[lo] ) ), true };
    }
} // namespace Desert::Editor::Sequencer
