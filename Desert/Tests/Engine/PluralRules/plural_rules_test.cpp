// WHAT THIS SUITE IS FOR, in one sentence: the plural form of a count is a property of the LANGUAGE, and
// almost everyone writes `n == 1 ? singular : plural` because that is a property of English.
//
// The numbers below are not samples somebody liked. They are the ones where a hand-rolled rule breaks:
//
//   ru   21 is `one` ("21 файл") and 11 is `many` ("11 файлов") — a threshold cannot express that
//   pl   21 is `many` ("21 plików") where Russian says `one`, so "the Slavic rule" is two rules
//   fr   0 is `one` ("0 fichier"), which is the case a UI shows most often
//   en   1.0 is `other` ("1.0 files") while 1 is `one` — the difference is in how it is PRINTED
//   ar   six categories, and 100/101 fall back to `other` while 103 is `few`
//
// The expectations are transcribed from Unicode CLDR release-48-2, common/supplemental/plurals.xml,
// <plurals type="cardinal">. They are not derived from the implementation; the sample ranges CLDR
// publishes alongside each rule were used to pick them.

#include <gtest/gtest.h>

#include <Engine/Localization/PluralRules.hpp>

#include <array>
#include <limits>
#include <set>

using namespace Desert::Localization;

namespace
{
    PluralCategory Of( const PluralRuleSet set, const double n, const int32_t v = 0 )
    {
        return SelectCardinal( set, n, v );
    }
} // namespace

TEST( PluralRules, RussianHasThreeFormsAndTheSplitIsNotAThreshold )
{
    // The six the brief named, and they are exactly the six that break a naive rule.
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 1 ), PluralCategory::One );    // 1 файл
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 2 ), PluralCategory::Few );    // 2 файла
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 5 ), PluralCategory::Many );   // 5 файлов
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 11 ), PluralCategory::Many );  // 11 файлов, NOT "one"
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 21 ), PluralCategory::One );   // 21 файл
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 111 ), PluralCategory::Many ); // 111 файлов

    // The teens are the whole reason `i % 100 != 11..14` is in the rule.
    for ( int n = 11; n <= 14; ++n )
        EXPECT_EQ( Of( PluralRuleSet::EastSlavic, n ), PluralCategory::Many ) << "n = " << n;
    for ( int n = 111; n <= 114; ++n )
        EXPECT_EQ( Of( PluralRuleSet::EastSlavic, n ), PluralCategory::Many ) << "n = " << n;

    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 0 ), PluralCategory::Many );
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 22 ), PluralCategory::Few );
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 25 ), PluralCategory::Many );
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 101 ), PluralCategory::One );

    // `other` is reachable ONLY through a printed fraction. A Russian table therefore needs three forms,
    // and a fourth only if it ever shows a fraction.
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 1.5, 1 ), PluralCategory::Other );
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 1.0, 1 ), PluralCategory::Other );
}

TEST( PluralRules, PolishIsNotRussian )
{
    EXPECT_EQ( Of( PluralRuleSet::Polish, 1 ), PluralCategory::One );
    EXPECT_EQ( Of( PluralRuleSet::Polish, 2 ), PluralCategory::Few );
    EXPECT_EQ( Of( PluralRuleSet::Polish, 5 ), PluralCategory::Many );
    EXPECT_EQ( Of( PluralRuleSet::Polish, 11 ), PluralCategory::Many );

    // THE DIVERGENCE. Russian calls 21 `one`; Polish calls it `many`. One shared "Slavic" implementation
    // would be wrong in one of the two languages and right in the other, which is the worst kind of wrong.
    EXPECT_EQ( Of( PluralRuleSet::Polish, 21 ), PluralCategory::Many );
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, 21 ), PluralCategory::One );
    EXPECT_NE( Of( PluralRuleSet::Polish, 21 ), Of( PluralRuleSet::EastSlavic, 21 ) );

    EXPECT_EQ( Of( PluralRuleSet::Polish, 22 ), PluralCategory::Few );
    EXPECT_EQ( Of( PluralRuleSet::Polish, 0 ), PluralCategory::Many );
    EXPECT_EQ( Of( PluralRuleSet::Polish, 112 ), PluralCategory::Many ); // i % 100 = 12..14
    EXPECT_EQ( Of( PluralRuleSet::Polish, 122 ), PluralCategory::Few );
}

TEST( PluralRules, ArabicHasSix )
{
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 0 ), PluralCategory::Zero );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 1 ), PluralCategory::One );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 2 ), PluralCategory::Two );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 3 ), PluralCategory::Few );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 10 ), PluralCategory::Few );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 11 ), PluralCategory::Many );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 99 ), PluralCategory::Many );
    // The modulo is on 100, so the sequence repeats per hundred and 100/101/102 come back round.
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 100 ), PluralCategory::Other );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 101 ), PluralCategory::Other );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 102 ), PluralCategory::Other );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 103 ), PluralCategory::Few );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 111 ), PluralCategory::Many );

    // `x = a..b` in CLDR is membership of an INTEGER SET, not of an interval: 3.5 is not `few`.
    EXPECT_EQ( Of( PluralRuleSet::Arabic, 3.5, 1 ), PluralCategory::Other );

    // All six categories are reachable — a rule set that could never produce one of its forms would make
    // that form of every Arabic translation dead text.
    const std::array<double, 6> witnesses = { 0, 1, 2, 3, 11, 100 };
    std::set<PluralCategory>    seen;
    for ( const double n : witnesses )
        seen.insert( Of( PluralRuleSet::Arabic, n ) );
    EXPECT_EQ( seen.size(), 6u );
}

TEST( PluralRules, EnglishReadsTheNumberOfPRINTEDDigits )
{
    EXPECT_EQ( Of( PluralRuleSet::OneOtherIntegerOnly, 1 ), PluralCategory::One );
    EXPECT_EQ( Of( PluralRuleSet::OneOtherIntegerOnly, 0 ), PluralCategory::Other );
    EXPECT_EQ( Of( PluralRuleSet::OneOtherIntegerOnly, 2 ), PluralCategory::Other );

    // "1 file" but "1.0 files". The double is the same; the printed form is not, and `v` is the only
    // operand that can tell them apart. A formatter that took only the value would say "1.0 file".
    EXPECT_EQ( Of( PluralRuleSet::OneOtherIntegerOnly, 1.0, 1 ), PluralCategory::Other );
}

TEST( PluralRules, FrenchCountsZeroAsSingular )
{
    EXPECT_EQ( Of( PluralRuleSet::FrenchOneMany, 0 ), PluralCategory::One );
    EXPECT_EQ( Of( PluralRuleSet::FrenchOneMany, 1 ), PluralCategory::One );
    EXPECT_EQ( Of( PluralRuleSet::FrenchOneMany, 2 ), PluralCategory::Other );
    EXPECT_EQ( Of( PluralRuleSet::FrenchOneMany, 1000000 ), PluralCategory::Many );
    EXPECT_EQ( Of( PluralRuleSet::FrenchOneMany, 1000001 ), PluralCategory::Other );
}

TEST( PluralRules, JapaneseHasOneForm )
{
    for ( const double n : { 0.0, 1.0, 2.0, 5.0, 11.0, 21.0, 111.0 } )
        EXPECT_EQ( Of( PluralRuleSet::OtherOnly, n ), PluralCategory::Other );
}

TEST( PluralRules, NegativeCountsUseTheirMagnitudeAndNonFiniteDoesNotCrash )
{
    // CLDR's `n` is the ABSOLUTE value. "-21 files" agrees with "21 files" in every language here.
    EXPECT_EQ( Of( PluralRuleSet::EastSlavic, -21 ), Of( PluralRuleSet::EastSlavic, 21 ) );
    EXPECT_EQ( Of( PluralRuleSet::Arabic, -3 ), Of( PluralRuleSet::Arabic, 3 ) );

    const PluralOperands nan = MakeOperands( std::numeric_limits<double>::quiet_NaN(), 0 );
    EXPECT_EQ( nan.I, 0u );
    EXPECT_EQ( nan.V, 0 );
}

TEST( PluralRules, TheNameTableAndItsInverseAgree )
{
    // The file format spells a form with these names, and the selector is chosen with this enum. Two
    // statements of one set is a fork waiting to happen, so the relation is asserted rather than trusted.
    for ( uint8_t i = 0; i <= static_cast<uint8_t>( PluralCategory::Other ); ++i )
    {
        const auto category = static_cast<PluralCategory>( i );
        const auto back     = PluralCategoryFromName( PluralCategoryName( category ) );
        ASSERT_TRUE( back.has_value() ) << PluralCategoryName( category );
        EXPECT_EQ( *back, category );
    }
    EXPECT_FALSE( PluralCategoryFromName( "singular" ).has_value() );
    EXPECT_FALSE( PluralCategoryFromName( "" ).has_value() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
