// The locale table is TRANSCRIBED DATA (Unicode CLDR release-48-2), so this suite has two halves that do
// different jobs.
//
//   1. SPOT VALUES against the CLDR source. A separator that silently became a plain space, or a currency
//      that grew a minor unit it does not have, is a difference nobody sees on the machine that made it.
//   2. RELATIONS over the whole table, which catch the row NOBODY thought to spot-check: every row's date
//      pattern must be one FormatDate can honour, every rule set must be implemented, every tag must be
//      findable by the function everything else uses to find it.
//
// The second half is the half that matters. A table is data, and data grows by rows; a suite that only
// knows the rows that existed when it was written stops testing the moment somebody adds one.

#include <gtest/gtest.h>

#include <Engine/Localization/LocaleFormat.hpp>

#include <cstdint>
#include <limits>
#include <set>
#include <string>

using namespace Desert::Localization;

namespace
{
    // The bytes, spelled out, so a failure message shows WHICH invisible character turned up. Three of
    // the seven rows separate groups with a space that is not a space.
    constexpr const char* kNbsp       = "\xC2\xA0";     // U+00A0
    constexpr const char* kNarrowNbsp = "\xE2\x80\xAF"; // U+202F

    const LocaleRow& Row( const char* tag )
    {
        const LocaleRow* row = FindLocale( tag );
        EXPECT_NE( row, nullptr ) << tag;
        return *row;
    }
} // namespace

TEST( LocaleFormat, NumbersUseTheLocalesOwnSeparators )
{
    EXPECT_EQ( FormatNumber( Row( "en" ), 1234567.5, 2 ), "1,234,567.50" );
    EXPECT_EQ( FormatNumber( Row( "de" ), 1234567.5, 2 ), "1.234.567,50" );
    EXPECT_EQ( FormatNumber( Row( "ru" ), 1234567.5, 2 ), std::string( "1" ) + kNbsp + "234" + kNbsp + "567,50" );
    EXPECT_EQ( FormatNumber( Row( "fr" ), 1234567.5, 2 ),
               std::string( "1" ) + kNarrowNbsp + "234" + kNarrowNbsp + "567,50" );

    // ar's default numbering system in CLDR 48 is `latn`, NOT `arab`: this is what the XML says, and the
    // obvious guess (Arabic-Indic digits) is wrong. Pinned because it is the row a future reader will
    // "correct".
    EXPECT_EQ( FormatNumber( Row( "ar" ), 1234.5, 1 ), "1,234.5" );
}

TEST( LocaleFormat, GroupingNeverLandsInsideTheLeadingGroup )
{
    // This engine has already shipped a thousands formatter that produced "1 2" for 12, because it
    // computed the first separator's position by SUBTRACTING in an unsigned type. Counting the digits
    // that REMAIN has no subtraction in it — and the property to assert is the relation, over every
    // magnitude, not a handful of round numbers (every multiple-of-three digit count was already right).
    const LocaleRow& en = Row( "en" );
    for ( int64_t v = 1; v <= 100000000; v *= 10 )
    {
        for ( const int64_t sample : { v, v + 1, v * 2 + 3 } )
        {
            const std::string text = FormatInteger( en, sample );
            std::string       digits;
            size_t            groups = 0;
            for ( const char c : text )
            {
                if ( c == ',' )
                    ++groups;
                else
                    digits.push_back( c );
            }
            EXPECT_EQ( digits, std::to_string( sample ) ) << text;
            EXPECT_EQ( groups, ( digits.size() - 1 ) / 3 ) << text;
            // No separator before the first digit, and none at the end.
            EXPECT_NE( text.front(), ',' );
            EXPECT_NE( text.back(), ',' );
        }
    }
    EXPECT_EQ( FormatInteger( en, 12 ), "12" );
    EXPECT_EQ( FormatInteger( en, 999 ), "999" );
    EXPECT_EQ( FormatInteger( en, 1000 ), "1,000" );
}

TEST( LocaleFormat, IntegersDoNotGoThroughADouble )
{
    // A player's money is exactly the number that goes past 2^53, and formatting it through a double
    // would round it. The two extremes of int64 are the witnesses.
    EXPECT_EQ( FormatInteger( Row( "en" ), 9007199254740993LL ), "9,007,199,254,740,993" );
    EXPECT_EQ( FormatInteger( Row( "en" ), std::numeric_limits<int64_t>::min() ), "-9,223,372,036,854,775,808" );
    EXPECT_EQ( FormatInteger( Row( "en" ), 0 ), "0" );
}

TEST( LocaleFormat, TheCurrencyDecidesTheDigitsAndTheLANGUAGEDecidesTheLayout )
{
    EXPECT_EQ( FormatCurrency( Row( "en" ), 1248500.0, *FindCurrency( "USD" ) ), "$1,248,500.00" );
    // Same money, German reader: the symbol moves and the separators change. That split is the point.
    EXPECT_EQ( FormatCurrency( Row( "de" ), 1248500.0, *FindCurrency( "USD" ) ),
               std::string( "1.248.500,00" ) + kNbsp + "$" );
    EXPECT_EQ( FormatCurrency( Row( "ru" ), 1248500.0, *FindCurrency( "RUB" ) ),
               std::string( "1" ) + kNbsp + "248" + kNbsp + "500,00" + kNbsp + "\xE2\x82\xBD" );
    // JPY has no minor unit (CLDR currencyData fractions, digits="0"). "1,248,500.00 Yen" is not a
    // rounding preference, it is wrong.
    EXPECT_EQ( FormatCurrency( Row( "ja" ), 1248500.0, *FindCurrency( "JPY" ) ), "\xC2\xA5"
                                                                                 "1,248,500" );
}

TEST( LocaleFormat, DatesFollowTheLocalesShortPattern )
{
    const CalendarDate day{ 2026, 9, 14 };
    // Bound to a NAMED result before unwrapping: `ResultStr::GetValue()` is deleted on an rvalue, which is
    // this codebase's own guard against unwrapping a temporary nobody checked.
    auto formatted = [&day]( const char* tag )
    {
        const auto result = FormatDate( Row( tag ), day );
        EXPECT_TRUE( result ) << tag << ": " << ( result ? "" : result.GetError() );
        return result ? result.GetValue() : std::string{};
    };

    EXPECT_EQ( formatted( "en" ), "9/14/26" );
    EXPECT_EQ( formatted( "ru" ), "14.09.2026" );
    EXPECT_EQ( formatted( "de" ), "14.09.26" );
    EXPECT_EQ( formatted( "ja" ), "2026/09/14" );
    EXPECT_EQ( formatted( "pl" ), "14.09.2026" );
    // The day/month ORDER is the whole point: 9/14 and 14.09 are the same date and would be two different
    // dates if the pattern were shared.
    EXPECT_NE( formatted( "en" ), formatted( "ru" ) );
}

TEST( LocaleFormat, EveryShippedRowIsOneTheFormattersCanHonour )
{
    // THE RELATION, and the half of this suite that keeps working when the table grows. Each assertion
    // names a way a new row could be added that compiles, loads, and then produces something wrong.
    std::set<std::string> tags;
    for ( const LocaleRow& row : Locales() )
    {
        EXPECT_FALSE( row.Tag.empty() );
        EXPECT_TRUE( tags.insert( std::string( row.Tag ) ).second ) << "duplicate tag " << row.Tag;

        // Findable by the ONE function everything else resolves a language with. A row nothing can find
        // is a language that exists in the table and nowhere else.
        EXPECT_EQ( FindLocale( row.Tag ), &row ) << row.Tag;
        EXPECT_EQ( FindLocale( std::string( row.Tag ) + "-XX" ), &row ) << "region subtag must be trimmed";

        EXPECT_FALSE( row.Endonym.empty() ) << row.Tag << " has no name to show in a picker";
        EXPECT_FALSE( row.DecimalSeparator.empty() ) << row.Tag;
        EXPECT_FALSE( row.GroupSeparator.empty() ) << row.Tag;
        EXPECT_NE( row.DecimalSeparator, row.GroupSeparator )
             << row.Tag << ": one separator doing both jobs makes every grouped decimal unreadable";

        // The date pattern is DATA, and FormatDate refuses a field it cannot honour. That refusal must be
        // unreachable for every row this build ships — which is a claim about the table, checkable here
        // and nowhere else.
        const auto formatted = FormatDate( row, CalendarDate{ 2026, 12, 31 } );
        EXPECT_TRUE( formatted ) << row.Tag << ": " << ( formatted ? "" : formatted.GetError() );

        // The currency pattern must place the amount exactly once, or money is printed twice or not at
        // all. Checked through the formatter with a value whose digits cannot occur by accident.
        const std::string money  = FormatCurrency( row, 7654321.0, *FindCurrency( "USD" ) );
        const std::string amount = FormatNumber( row, 7654321.0, 2 );
        size_t            at = money.find( amount ), times = 0;
        while ( at != std::string::npos )
        {
            ++times;
            at = money.find( amount, at + amount.size() );
        }
        EXPECT_EQ( times, 1u ) << row.Tag << ": the amount is printed " << times << " times in '" << money << "'";
        EXPECT_NE( money.find( "$" ), std::string::npos ) << row.Tag << ": the symbol vanished";
    }
    EXPECT_FALSE( tags.empty() );
}

TEST( LocaleFormat, EveryShippedRuleSetIsImplemented )
{
    // A row naming a rule set whose selector has no case would silently get `other` for every count,
    // which is "this language has no plurals" written as a defect.
    for ( const LocaleRow& row : Locales() )
    {
        std::set<PluralCategory> produced;
        for ( double n = 0; n <= 130; ++n )
            produced.insert( SelectCardinal( row.Plural, n, 0 ) );
        produced.insert( SelectCardinal( row.Plural, 1.5, 1 ) );
        EXPECT_FALSE( produced.empty() ) << row.Tag;
        // Only Japanese is allowed to have exactly one form over that range; anything else with one form
        // is a rule set that was never wired up.
        if ( row.Plural != PluralRuleSet::OtherOnly )
            EXPECT_GT( produced.size(), 1u ) << row.Tag << " selects one category for every count";
    }
}

TEST( LocaleFormat, UnknownTagsAndCurrenciesAreMissesAndNotGuesses )
{
    EXPECT_EQ( FindLocale( "kl" ), nullptr );
    EXPECT_EQ( FindLocale( "" ), nullptr );
    EXPECT_EQ( FindLocale( "-RU" ), nullptr );
    EXPECT_EQ( FindCurrency( "XXX" ), nullptr );
    // Case and separator shape must not decide the answer — an OS hands out "ru_RU" and a browser "ru-RU".
    EXPECT_EQ( FindLocale( "RU" ), FindLocale( "ru" ) );
    EXPECT_EQ( FindLocale( "ru_RU" ), FindLocale( "ru-RU" ) );
    EXPECT_EQ( FindCurrency( "usd" ), FindCurrency( "USD" ) );
}

TEST( LocaleFormat, NonFiniteNumbersAnnounceThemselves )
{
    // A NaN reaching a label is a defect upstream, and the label is the only place it can say so. An
    // empty string or a quiet "0" would hide it.
    EXPECT_EQ( FormatNumber( Row( "en" ), std::numeric_limits<double>::quiet_NaN(), 2 ), "NaN" );
    EXPECT_EQ( FormatNumber( Row( "en" ), std::numeric_limits<double>::infinity(), 2 ), "Inf" );
    // -0.4 with no decimals rounds to zero, and "-0" is not a number anybody meant to show.
    EXPECT_EQ( FormatNumber( Row( "en" ), -0.4, 0 ), "0" );
    EXPECT_EQ( FormatNumber( Row( "en" ), -1.5, 0 ), "-2" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
