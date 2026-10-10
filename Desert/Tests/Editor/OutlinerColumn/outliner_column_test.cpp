// The Sequencer's Outliner column (ANIM-FIX3b2): a row's name is fitted to the names column with "…" (the
// panel shows the whole name in the tooltip), and the column's width — the panel's state, dragged by the
// splitter — stays between its minimum and what leaves the controls column and the lanes.

#include <Editor/Panels/Sequencer/OutlinerColumn.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{
    namespace SQ = Desert::Editor::Sequencer;

    // A fixed advance per code point: 10 px.
    float Measure( const std::string_view text )
    {
        float width = 0.0f;
        for ( const char c : text )
            if ( ( static_cast<unsigned char>( c ) & 0xC0u ) != 0x80u )
                width += 10.0f;
        return width;
    }

    std::string Dots( const std::string& prefix )
    {
        return prefix + std::string( SQ::kEllipsis );
    }
} // namespace

TEST( OutlinerColumn, NameThatFitsIsUntouched )
{
    const auto fitted = SQ::FitLabel( "Sphere", 60.0f, Measure );
    EXPECT_EQ( fitted.Text, "Sphere" );
    EXPECT_FALSE( fitted.Truncated );
}

TEST( OutlinerColumn, LongNameEndsInEllipsisWithinTheWidth )
{
    // The live defect's label: "Slot 0 (MP_Default) ▸ Blend" in a column that held "Slot 0 (MP_Defau".
    const std::string label  = "Slot 0 (MP_Default) \xE2\x96\xB8 Blend";
    const auto        fitted = SQ::FitLabel( label, 100.0f, Measure );
    EXPECT_TRUE( fitted.Truncated );
    EXPECT_EQ( fitted.Text, Dots( "Slot 0 (M" ) ); // 9 code points + "…" = 100 px
    EXPECT_LE( Measure( fitted.Text ), 100.0f );
}

TEST( OutlinerColumn, CutFallsBetweenCodePointsAndDropsTrailingSpaces )
{
    // "ab ▸ cd": at 50 px the prefix "ab ▸" (4) + "…" fits; at 40 px "ab " + "…" — the space is dropped.
    const std::string label = "ab \xE2\x96\xB8 cd";
    EXPECT_EQ( SQ::FitLabel( label, 50.0f, Measure ).Text, Dots( "ab \xE2\x96\xB8" ) );
    EXPECT_EQ( SQ::FitLabel( label, 40.0f, Measure ).Text, Dots( "ab" ) );
}

TEST( OutlinerColumn, NoRoomLeavesTheEllipsisAlone )
{
    const auto fitted = SQ::FitLabel( "Visibility", 5.0f, Measure );
    EXPECT_EQ( fitted.Text, std::string( SQ::kEllipsis ) );
    EXPECT_TRUE( fitted.Truncated );
}

TEST( OutlinerColumn, NameWidthIsClampedToMinimumAndToTheLanes )
{
    const float panel  = 1000.0f;
    const float widest = panel - SQ::OutlinerColumn::kControlsWidth - SQ::OutlinerColumn::kMinLaneWidth;
    EXPECT_EQ( SQ::ClampNameColumnWidth( 300.0f, panel ), 300.0f );
    EXPECT_EQ( SQ::ClampNameColumnWidth( 10.0f, panel ), SQ::OutlinerColumn::kMinNameWidth );
    EXPECT_EQ( SQ::ClampNameColumnWidth( 5000.0f, panel ), widest );
    // A panel too narrow for the controls and the lanes keeps the minimum names column.
    EXPECT_EQ( SQ::ClampNameColumnWidth( 300.0f, 200.0f ), SQ::OutlinerColumn::kMinNameWidth );
}
