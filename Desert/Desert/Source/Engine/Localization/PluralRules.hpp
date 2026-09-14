#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Desert::Localization
{
    /**
     * @file
     * @brief The CLDR cardinal plural rules, transcribed — not invented.
     *
     * WHERE EVERY RULE IN THIS FILE COMES FROM. Unicode CLDR, tag `release-48-2`, file
     * `common/supplemental/plurals.xml`, `<plurals type="cardinal">`. SPDX `Unicode-3.0`; the rules are
     * DATA read out of that file, no CLDR code is linked and no library is vendored. The `.cpp` beside
     * this header quotes the rule text for every set it implements, verbatim, so the transcription can be
     * checked against the source without leaving the file.
     *
     * WHY THIS IS NOT "SINGULAR AND PLURAL". English has two forms and that is a property of English, not
     * of counting. Russian has three (1 файл / 2 файла / 5 файлов) and the selector is NOT `n == 1`: 21 is
     * `one` and 11 is `many`, which is the case every hand-rolled `n == 1 ? a : b` gets wrong. Polish
     * splits differently again, and Arabic has six. A formatter that cannot express that produces text
     * that is wrong in a way its author cannot see, because their own language happens to be the easy one.
     *
     * THE OPERANDS ARE CLDR'S OPERANDS, and the two that matter here are `n` (the absolute value) and `v`
     * (the number of VISIBLE fraction digits — a property of how the number will be PRINTED, not of the
     * double). "1 file" and "1.0 files" differ in English, and `v` is the only thing that tells them
     * apart, so the caller has to say how many digits it is about to show. `e` (the compact-notation
     * exponent) is always 0 here because this engine has no compact notation; the two rules that read it
     * are written out with that substitution stated at the site.
     */

    /// The six CLDR categories. NOT a language's form count: a language uses the subset its rule set
    /// selects, and `Other` is the only one every language has.
    enum class PluralCategory : uint8_t
    {
        Zero,
        One,
        Two,
        Few,
        Many,
        Other
    };

    /// The category's CLDR spelling, which is also the spelling a `.destrings` file uses for a form.
    /// One name table, so the file format and the selector cannot drift apart.
    constexpr std::string_view PluralCategoryName( const PluralCategory category )
    {
        switch ( category )
        {
            case PluralCategory::Zero:
                return "zero";
            case PluralCategory::One:
                return "one";
            case PluralCategory::Two:
                return "two";
            case PluralCategory::Few:
                return "few";
            case PluralCategory::Many:
                return "many";
            case PluralCategory::Other:
                return "other";
        }
        // Unreachable for every enumerator above (no `default:`, so -Wswitch reports a new one here).
        return "other";
    }

    /// The inverse, for reading a form name out of a file. Nothing on a miss — an unknown form name is a
    /// refusal the table parser reports by name, never a silent fall-through to `other`.
    std::optional<PluralCategory> PluralCategoryFromName( std::string_view name );

    /**
     * @brief A whole language's cardinal rule, named after the shape rather than after one language.
     *
     * Several languages share a rule set exactly (CLDR groups them in one `<pluralRules>` element), and
     * naming the set after its shape is what stops a second language being given a second copy of the
     * same code. The comment on each enumerator lists the CLDR locales in that element that this engine
     * ships a row for; the file lists more, and adding one is a row in LocaleRegistry, not a case here.
     */
    enum class PluralRuleSet : uint8_t
    {
        /// One form, ever. CLDR element `bm bo dz ... ja ... ko ... zh`. Shipped: ja.
        OtherOnly,
        /// `one: i = 1 and v = 0`. CLDR element `ast de en et fi ...`. Shipped: en, de.
        OneOtherIntegerOnly,
        /// `one: i = 0,1` plus a `many` for whole millions. CLDR element `fr pt`. Shipped: fr.
        FrenchOneMany,
        /// The east-Slavic three. CLDR element `ru uk`. Shipped: ru.
        EastSlavic,
        /// Polish — the same three names as ru, a different split. CLDR element `pl`. Shipped: pl.
        Polish,
        /// Six categories. CLDR element `ar ars`. Shipped: ar.
        Arabic
    };

    /**
     * @brief CLDR's plural operands, reduced to the three this engine can supply honestly.
     *
     * `N` is |value|. `I` is its integer part. `V` is the count of fraction digits that will be PRINTED,
     * which the caller knows and the value does not.
     *
     * `F`/`T`/`W` are absent because no shipped rule reads them; `E` is absent because it is 0 for every
     * number this engine formats (no compact notation) and a field that is always one value is a field
     * that will be trusted the first time it is not.
     */
    struct PluralOperands
    {
        double   N = 0.0;
        uint64_t I = 0;
        int32_t  V = 0;
    };

    /// The operands of @p value printed with @p fractionDigits digits after the point. Negative values use
    /// their absolute value, as CLDR's `n` does; a non-finite value is reported as 0 with no fraction,
    /// because "NaN files" has no plural form in any language and the caller has a bigger problem.
    PluralOperands MakeOperands( double value, int32_t fractionDigits );

    /// The category @p set selects for @p operands. Total — every set has an `Other` and returns it when
    /// no earlier rule matches, exactly as CLDR specifies.
    PluralCategory SelectCardinal( PluralRuleSet set, const PluralOperands& operands );

    /// Convenience for the common call: an integer count printed with no fraction digits.
    inline PluralCategory SelectCardinal( const PluralRuleSet set, const double value,
                                          const int32_t fractionDigits = 0 )
    {
        return SelectCardinal( set, MakeOperands( value, fractionDigits ) );
    }
} // namespace Desert::Localization
