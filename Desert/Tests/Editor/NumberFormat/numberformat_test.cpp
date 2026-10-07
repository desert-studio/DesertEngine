// THE DEFECT THIS SUITE PINS, which shipped and was on screen in the Details panel's Mesh section.
//
// The thousands formatter computed where the first separator goes as `lead = size % 3` and then tested
// `( i - lead ) % 3 == 0`. Both operands are size_t, so for any i < lead the subtraction WRAPPED to near
// 2^64 -- and 2^64-1 is divisible by 3, so the test fired and a space landed inside the leading group.
//
//   12       -> "1 2"
//   999      -> "99 9"
//   13348    -> "1 3 348"
//   148902   -> "14 8 902"
//
// It was correct exactly when the digit count was a multiple of three, which is one magnitude in three
// and includes every round example anyone would have checked by hand (1000, 1489020). Nothing failed;
// the number was simply wrong, in a readout whose entire purpose is to be read.
//
// The rule is a RELATION between the string and the number of digits, so that is what is asserted:
// separators sit exactly at the positions where the remaining digit count is a multiple of three, and
// stripping them returns the original digits.

#include <gtest/gtest.h>

#include <Editor/Core/NumberFormat.hpp>

#include <algorithm>
#include <string>

using Desert::Editor::FormatThousands;

TEST( NumberFormat, GroupsFromTheRight )
{
    EXPECT_EQ( FormatThousands( 0 ), "0" );
    EXPECT_EQ( FormatThousands( 7 ), "7" );
    EXPECT_EQ( FormatThousands( 12 ), "12" );      // was "1 2"
    EXPECT_EQ( FormatThousands( 24 ), "24" );      // was "2 4"
    EXPECT_EQ( FormatThousands( 999 ), "999" );    // was "99 9"
    EXPECT_EQ( FormatThousands( 1000 ), "1 000" ); // was already right -- the third of cases that hid it
    EXPECT_EQ( FormatThousands( 13348 ), "13 348" );
    EXPECT_EQ( FormatThousands( 148902 ), "148 902" );
    EXPECT_EQ( FormatThousands( 1489020 ), "1 489 020" );
    EXPECT_EQ( FormatThousands( 12345678 ), "12 345 678" );
}

TEST( NumberFormat, EveryDigitCountIsGroupedCorrectly )
{
    // Sweep the magnitudes rather than sampling them: the bug was a function of size % 3, so it needed
    // one case per residue at several lengths to be visible at all.
    uint64_t value = 1;
    for ( int digits = 1; digits <= 19; ++digits )
    {
        const std::string formatted = FormatThousands( value );

        std::string bare;
        int         spaces = 0;
        for ( std::size_t i = 0; i < formatted.size(); ++i )
        {
            if ( formatted[i] == ' ' )
            {
                ++spaces;
                // A separator is only ever legal where the digits still to come are a multiple of three.
                const std::size_t remaining =
                     formatted.size() - i - 1 -
                     static_cast<std::size_t>( std::count( formatted.begin() + i + 1, formatted.end(), ' ' ) );
                EXPECT_EQ( remaining % 3, 0u ) << "digits=" << digits << " -> '" << formatted << "'";
            }
            else
            {
                bare += formatted[i];
            }
        }

        // Nothing added, nothing lost.
        EXPECT_EQ( bare, std::to_string( value ) ) << "digits=" << digits;
        EXPECT_EQ( spaces, ( digits - 1 ) / 3 ) << "digits=" << digits << " -> '" << formatted << "'";
        // A separator never opens or closes the string.
        EXPECT_NE( formatted.front(), ' ' ) << formatted;
        EXPECT_NE( formatted.back(), ' ' ) << formatted;

        if ( digits < 19 )
            value = value * 10 + static_cast<uint64_t>( digits % 10 );
    }
}

TEST( NumberFormat, HandlesTheWholeRange )
{
    EXPECT_EQ( FormatThousands( UINT64_MAX ), "18 446 744 073 709 551 615" );
}
