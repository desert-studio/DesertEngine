#pragma once

#include <Engine/Localization/PluralRules.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Localization
{
    /**
     * @file
     * @brief What a language IS, to this engine: one row of data, and the three formatters that read it.
     *
     * WHERE THE DATA COMES FROM. Unicode CLDR, tag `release-48-2` (SPDX `Unicode-3.0`), transcribed by
     * hand from `common/main/<tag>.xml` — `numbers/symbols[@numberSystem='latn']` for the separators,
     * `dates/calendars/calendar[@type='gregorian']/dateFormats/dateFormatLength[@type='short']` for the
     * date pattern, and `numbers/currencyFormats/.../pattern` for the currency layout. Every row below
     * carries the XPath value it was copied from. NOTHING IS VENDORED AND NOTHING IS LINKED: ICU is a
     * 30 MB dependency with a data blob, and what an engine UI needs from it is this table.
     *
     * WHAT IS DELIBERATELY NOT HERE, so that nobody looks for it: month and weekday NAMES. They are
     * ~20 strings per language of CLDR display data, they have no consumer in this engine (the only date
     * this UI can show is a numeric one), and transcribing 140 strings nobody reads is how a table starts
     * being wrong without anyone noticing. `FormatDate` therefore implements the numeric pattern only and
     * REFUSES a pattern containing `MMM` rather than inventing a name — see its comment.
     *
     * WHERE A FONT PER LANGUAGE WILL GO. Here, in `LocaleRow`, as one more member — the row is already
     * the per-language fact table, and a text draw already has the active row in hand. It is NOT in this
     * task because the glyph side of it (baking, atlas, the font asset's own format) is Ю14's, and a
     * field nothing reads is a dead setting (DEV_CONTRACT §1.3). When MSDF lands, the change is: a
     * `DefaultFont` member on this struct, a value in the seven rows below, and one lookup where
     * `UICanvasRenderer2D::DrawText2D` currently resolves `UITextData::Font`.
     */

    /// One language this engine can present. A `std::string_view` per field because every value is a
    /// string literal in the table — the rows are static data, not something anyone constructs.
    struct LocaleRow
    {
        /// The BCP-47 language subtag, lower-case. The identity everything else keys on: the `.destrings`
        /// files, the `--language` flag, the editor's language menu.
        std::string_view Tag;

        /// What speakers of this language call it, in this language. Shown in the language picker, because
        /// a menu that lists "Russian" to somebody who only reads Russian is a menu they cannot use.
        std::string_view Endonym;

        /// CLDR cardinal rule set (see PluralRules.hpp).
        PluralRuleSet Plural;

        /// `numbers/symbols[@numberSystem='latn']/decimal`.
        std::string_view DecimalSeparator;

        /// `numbers/symbols[@numberSystem='latn']/group`. Several of these are a NO-BREAK SPACE and one is
        /// a NARROW no-break space, and they are different characters — this is data, not whitespace.
        std::string_view GroupSeparator;

        /// The gregorian `short` date pattern, in CLDR pattern syntax (`d`, `dd`, `M`, `MM`, `y`, `yy`,
        /// literals, `'quoted'` literals). Numeric only — see the file comment.
        std::string_view DatePattern;

        /// The currency format pattern, CLDR syntax, where `\xC2\xA4` (U+00A4 CURRENCY SIGN) stands for the
        /// currency's own symbol. Carries the symbol's SIDE and the space before it as one value, because
        /// they are one fact about the language and two fields would let them disagree.
        std::string_view CurrencyPattern;
    };

    /// Every language this build ships, in the order a picker should list them. Never empty.
    std::span<const LocaleRow> Locales();

    /// The row for @p tag, or nullptr. Case-insensitive, and a region subtag is TRIMMED: "ru-RU" and
    /// "ru_RU" both find `ru`, because a caller passing an OS locale should reach the language it names
    /// rather than be told the language does not exist. A tag whose LANGUAGE part is unknown is a miss,
    /// and every caller reports the miss by name — there is no nearest-neighbour guess here.
    const LocaleRow* FindLocale( std::string_view tag );

    /// One currency this engine can print. Symbol and fraction digits from CLDR `common/supplemental/
    /// supplementalData.xml` (`currencyData/fractions`) and the root `currencies` display data.
    struct CurrencyRow
    {
        std::string_view Code;   ///< ISO 4217, upper-case ("USD").
        std::string_view Symbol; ///< The global symbol, not a locale-specific one (see FormatCurrency).
        int32_t          FractionDigits = 2;
    };

    std::span<const CurrencyRow> Currencies();
    const CurrencyRow*           FindCurrency( std::string_view code );

    /// A date with no clock and no zone — the only kind this engine formats. Fields are as written on a
    /// calendar (Month 1..12, Day 1..31), NOT `tm`'s offsets, because an off-by-one in a month is
    /// invisible eleven times in twelve.
    struct CalendarDate
    {
        int32_t Year  = 1970;
        int32_t Month = 1;
        int32_t Day   = 1;
    };

    /**
     * @brief @p value with @p fractionDigits digits, grouped and pointed as @p locale does it.
     *
     * Grouping is every three digits from the right, which is true of all seven shipped rows and is NOT
     * universal — `hi` groups 3 then 2 — so the day a row needs that, the grouping pattern becomes a
     * member of LocaleRow and this function reads it. Stated here rather than discovered later.
     */
    std::string FormatNumber( const LocaleRow& locale, double value, int32_t fractionDigits = 0 );

    /// Integer overload, so a count does not have to travel through a double to be printed.
    std::string FormatInteger( const LocaleRow& locale, int64_t value );

    /**
     * @brief @p amount of @p currency, laid out as @p locale lays out money.
     *
     * The LOCALE decides the layout (which side the symbol is on, the separators); the CURRENCY decides
     * the symbol and how many digits it has. That split is why an amount in dollars shown to a German
     * reads "1.234,50 $" and not "$1,234.50" — the language is German, the money is not.
     *
     * The symbol is the currency's GLOBAL one. CLDR also carries per-locale symbols ("US$" for USD in
     * most non-English locales), and this engine does not: that is 7 rows x N currencies of display data
     * with no consumer, and the same refusal as month names.
     */
    std::string FormatCurrency( const LocaleRow& locale, double amount, const CurrencyRow& currency );

    /**
     * @brief @p date under @p locale's short numeric pattern.
     *
     * REFUSES BY NAME a pattern field this build cannot honour — today that is any pattern asking for a
     * month NAME (`MMM` and longer), a weekday (`E`), or an era (`G`). A formatter that quietly printed
     * the month NUMBER where a name was asked for would produce "3.4.2026" for a locale whose readers
     * expect "3 Apr 2026", and nothing would ever say so.
     *
     * The refusal is unreachable for every row this build ships — LocaleFormat's suite asserts exactly
     * that over the whole table — so it is not an error path anybody has to handle in practice. It exists
     * because the table is DATA and the day a row gains `MMM` it must fail loudly rather than lie.
     */
    NO_DISCARD Common::ResultStr<std::string> FormatDate( const LocaleRow& locale, const CalendarDate& date );
} // namespace Desert::Localization
