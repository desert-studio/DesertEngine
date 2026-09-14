#include "PluralRules.hpp"

#include <cmath>

namespace Desert::Localization
{
    std::optional<PluralCategory> PluralCategoryFromName( const std::string_view name )
    {
        // Derived from the same name table the writer uses, so a category added to the enum is readable
        // the moment it is writable. A hand-written second list is how a form name becomes legal to write
        // and impossible to read back.
        for ( uint8_t i = 0; i <= static_cast<uint8_t>( PluralCategory::Other ); ++i )
        {
            const auto category = static_cast<PluralCategory>( i );
            if ( PluralCategoryName( category ) == name )
                return category;
        }
        return std::nullopt;
    }

    PluralOperands MakeOperands( const double value, const int32_t fractionDigits )
    {
        PluralOperands operands;
        if ( !std::isfinite( value ) )
            return operands;

        operands.N = std::fabs( value );
        operands.I = static_cast<uint64_t>( std::floor( operands.N ) );
        operands.V = fractionDigits < 0 ? 0 : fractionDigits;
        return operands;
    }

    namespace
    {
        // The two range helpers CLDR's rule syntax is written in. `a..b` is INCLUSIVE at both ends, which
        // is why 12..14 excludes 15 and includes 12 — the single most common transcription slip.
        constexpr bool InRange( const uint64_t v, const uint64_t low, const uint64_t high )
        {
            return v >= low && v <= high;
        }
    } // namespace

    PluralCategory SelectCardinal( const PluralRuleSet set, const PluralOperands& op )
    {
        // Every branch below is the CLDR rule text quoted above it. `e` is 0 throughout (see the header),
        // so `e = 0` is dropped and `e != 0..5` is dropped as always-false; both substitutions are stated
        // where they are made rather than silently applied.
        switch ( set )
        {
            case PluralRuleSet::OtherOnly:
                // CLDR: (no rule but `other`).
                return PluralCategory::Other;

            case PluralRuleSet::OneOtherIntegerOnly:
                // CLDR: one: i = 1 and v = 0
                //
                // `v = 0` is the half that a naive `value == 1.0` drops, and it is not pedantry: English
                // says "1 file" and "1.0 files". The distinction only exists in how the number is printed,
                // which is why V is an operand and not a property of the double.
                if ( op.I == 1 && op.V == 0 )
                    return PluralCategory::One;
                return PluralCategory::Other;

            case PluralRuleSet::FrenchOneMany:
                // CLDR: one:  i = 0,1
                //       many: e = 0 and i != 0 and i % 1000000 = 0 and v = 0 or e != 0..5
                //
                // French counts 0 as singular ("0 fichier"), which is the rule an English-shaped
                // formatter gets wrong on the empty case — the case a UI shows most often.
                if ( op.I == 0 || op.I == 1 )
                    return PluralCategory::One;
                if ( op.I != 0 && op.I % 1000000 == 0 && op.V == 0 )
                    return PluralCategory::Many;
                return PluralCategory::Other;

            case PluralRuleSet::EastSlavic:
                // CLDR: one:  v = 0 and i % 10 = 1 and i % 100 != 11
                //       few:  v = 0 and i % 10 = 2..4 and i % 100 != 12..14
                //       many: v = 0 and i % 10 = 0 or v = 0 and i % 10 = 5..9 or v = 0 and i % 100 = 11..14
                //
                // `other` is reachable ONLY through a fractional value (v != 0): "1,5 файла". Every whole
                // number lands in one of the three above, which is why a Russian table needs three forms
                // and a fourth only if it ever shows a fraction.
                if ( op.V == 0 )
                {
                    if ( op.I % 10 == 1 && op.I % 100 != 11 )
                        return PluralCategory::One;
                    if ( InRange( op.I % 10, 2, 4 ) && !InRange( op.I % 100, 12, 14 ) )
                        return PluralCategory::Few;
                    if ( op.I % 10 == 0 || InRange( op.I % 10, 5, 9 ) || InRange( op.I % 100, 11, 14 ) )
                        return PluralCategory::Many;
                }
                return PluralCategory::Other;

            case PluralRuleSet::Polish:
                // CLDR: one:  i = 1 and v = 0
                //       few:  v = 0 and i % 10 = 2..4 and i % 100 != 12..14
                //       many: v = 0 and i != 1 and i % 10 = 0..1 or v = 0 and i % 10 = 5..9
                //             or v = 0 and i % 100 = 12..14
                //
                // NOT the Russian split, and the difference is exactly 21: Russian calls it `one`
                // ("21 плик" is wrong in Polish), Polish calls it `many` ("21 plików"). One shared
                // implementation for "the Slavic languages" would be wrong in one of the two.
                if ( op.I == 1 && op.V == 0 )
                    return PluralCategory::One;
                if ( op.V == 0 )
                {
                    if ( InRange( op.I % 10, 2, 4 ) && !InRange( op.I % 100, 12, 14 ) )
                        return PluralCategory::Few;
                    if ( ( op.I != 1 && InRange( op.I % 10, 0, 1 ) ) || InRange( op.I % 10, 5, 9 ) ||
                         InRange( op.I % 100, 12, 14 ) )
                        return PluralCategory::Many;
                }
                return PluralCategory::Other;

            case PluralRuleSet::Arabic:
                // CLDR: zero: n = 0   one: n = 1   two: n = 2
                //       few:  n % 100 = 3..10      many: n % 100 = 11..99
                //
                // Six forms, and they are tested on n rather than on i — so 100 and 101 are `other`,
                // 103 is `few`, and 111 is `many`. The modulo is on 100, so the sequence repeats per
                // hundred and cannot be approximated by any threshold.
                {
                    if ( op.N == 0.0 )
                        return PluralCategory::Zero;
                    if ( op.N == 1.0 )
                        return PluralCategory::One;
                    if ( op.N == 2.0 )
                        return PluralCategory::Two;
                    // `n % 100` on a possibly fractional n. CLDR's `x = a..b` tests membership of the
                    // INTEGER SET {a..b} (TR35 §Language Plural Rules), so 3.5 is not `few` however
                    // close it sits to 3 — a `>= 3.0 && <= 10.0` transcription would silently widen the
                    // rule to every fraction in between. std::fmod keeps the fraction, and the integrality
                    // test is what turns the interval back into the set.
                    const double mod100  = std::fmod( op.N, 100.0 );
                    const bool   isWhole = mod100 == std::floor( mod100 );
                    if ( isWhole && mod100 >= 3.0 && mod100 <= 10.0 )
                        return PluralCategory::Few;
                    if ( isWhole && mod100 >= 11.0 && mod100 <= 99.0 )
                        return PluralCategory::Many;
                    return PluralCategory::Other;
                }
        }
        // Unreachable for every enumerator above; a rule set added without a case reaches here rather
        // than borrowing the previous one's answer.
        return PluralCategory::Other;
    }
} // namespace Desert::Localization
