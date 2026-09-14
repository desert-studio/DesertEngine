#include "LocaleFormat.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace Desert::Localization
{
    namespace
    {
        // The seven rows, each with the CLDR XPath value it was copied from. Order is the order a picker
        // lists them: the two this repository's content is authored in first, then the rest alphabetically
        // by tag, so adding a language is an insertion with an obvious place rather than a judgement.
        //
        // U+00A0 NO-BREAK SPACE is "\xC2\xA0" and U+202F NARROW NO-BREAK SPACE is "\xE2\x80\xAF"; U+200F
        // RIGHT-TO-LEFT MARK is "\xE2\x80\x8F" and U+00A4 CURRENCY SIGN is "\xC2\xA4". They are written as
        // BYTES rather than as characters because this workspace does not pass `/utf-8` to MSVC, which
        // therefore reads a source file in the machine's active code page: a literal non-ASCII character
        // here would be re-encoded on Windows and be a different string in the two builds. (The same
        // class as the other Windows-only defects this project has paid for.)
        constexpr std::array<LocaleRow, 7> kLocales = {
             LocaleRow{ "en", "English", PluralRuleSet::OneOtherIntegerOnly, ".", ",", "M/d/yy",
                        "\xC2\xA4#,##0.00" },
             LocaleRow{ "ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9",
                        PluralRuleSet::EastSlavic, ",", "\xC2\xA0", "dd.MM.y", "#,##0.00\xC2\xA0\xC2\xA4" },
             LocaleRow{ "ar", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9", PluralRuleSet::Arabic,
                        ".", ",", "d\xE2\x80\x8F/M\xE2\x80\x8F/y", "\xE2\x80\x8F#,##0.00\xC2\xA0\xC2\xA4" },
             LocaleRow{ "de", "Deutsch", PluralRuleSet::OneOtherIntegerOnly, ",", ".", "dd.MM.yy",
                        "#,##0.00\xC2\xA0\xC2\xA4" },
             LocaleRow{ "fr",
                        "Fran\xC3\xA7"
                        "ais",
                        PluralRuleSet::FrenchOneMany, ",", "\xE2\x80\xAF", "dd/MM/y", "#,##0.00\xC2\xA0\xC2\xA4" },
             LocaleRow{ "ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", PluralRuleSet::OtherOnly, ".", ",",
                        "y/MM/dd", "\xC2\xA4#,##0.00" },
             LocaleRow{ "pl", "Polski", PluralRuleSet::Polish, ",", "\xC2\xA0", "d.MM.y",
                        "#,##0.00\xC2\xA0\xC2\xA4" },
        };

        // `ar`'s default numbering system in CLDR 48 is `latn`, NOT `arab`: Arabic-Indic digits are the
        // NATIVE system and the default only in some regional locales (ar-EG). The separators above are
        // therefore root's latin ones, which is what the XML says and not what one would guess. Written
        // down because guessing the other way is the obvious mistake and it would be invisible to anyone
        // who cannot read the result.

        constexpr std::array<CurrencyRow, 6> kCurrencies = {
             CurrencyRow{ "USD", "$", 2 },
             CurrencyRow{ "EUR", "\xE2\x82\xAC", 2 },
             CurrencyRow{ "GBP", "\xC2\xA3", 2 },
             CurrencyRow{ "RUB", "\xE2\x82\xBD", 2 },
             CurrencyRow{ "PLN", "z\xC5\x82", 2 },
             // JPY has NO minor unit — CLDR `currencyData/fractions/info[@iso4217='JPY'][@digits='0']`.
             // "1 234,00 Yen" is not a rounding preference, it is wrong.
             CurrencyRow{ "JPY", "\xC2\xA5", 0 },
        };

        char LowerAscii( const char c )
        {
            return ( c >= 'A' && c <= 'Z' ) ? static_cast<char>( c - 'A' + 'a' ) : c;
        }

        char UpperAscii( const char c )
        {
            return ( c >= 'a' && c <= 'z' ) ? static_cast<char>( c - 'a' + 'A' ) : c;
        }

        bool EqualsFold( const std::string_view a, const std::string_view b, const bool upper )
        {
            if ( a.size() != b.size() )
            {
                return false;
            }
            for ( size_t i = 0; i < a.size(); ++i )
            {
                const char x = upper ? UpperAscii( a[i] ) : LowerAscii( a[i] );
                const char y = upper ? UpperAscii( b[i] ) : LowerAscii( b[i] );
                if ( x != y )
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    std::span<const LocaleRow> Locales()
    {
        return { kLocales.data(), kLocales.size() };
    }

    const LocaleRow* FindLocale( const std::string_view tag )
    {
        // The LANGUAGE subtag only. A caller holding "ru-RU", "ru_RU" or "RU" is asking for Russian, and
        // answering "no such language" would be true of the string and false of the question.
        const size_t cut      = tag.find_first_of( "-_" );
        const auto   language = tag.substr( 0, cut == std::string_view::npos ? tag.size() : cut );
        if ( language.empty() )
        {
            return nullptr;
        }

        for ( const LocaleRow& row : kLocales )
        {
            if ( EqualsFold( row.Tag, language, /*upper=*/false ) )
                return &row;
        }
        return nullptr;
    }

    std::span<const CurrencyRow> Currencies()
    {
        return { kCurrencies.data(), kCurrencies.size() };
    }

    const CurrencyRow* FindCurrency( const std::string_view code )
    {
        for ( const CurrencyRow& row : kCurrencies )
        {
            if ( EqualsFold( row.Code, code, /*upper=*/true ) )
                return &row;
        }
        return nullptr;
    }

    namespace
    {
        // The digits of |value| rounded to `fractionDigits`, as one run of ASCII digits plus the position
        // of the decimal point inside it. Done through snprintf rather than by hand because rounding a
        // double to a given number of decimals correctly is the part everybody gets wrong, and the
        // standard library already has it.
        struct DigitRun
        {
            std::string Digits;     // "123450" for 1234.50
            size_t      IntegerLen; // 4
        };

        DigitRun RoundedDigits( const double magnitude, const int32_t fractionDigits )
        {
            char      buffer[512];
            const int written =
                 std::snprintf( buffer, sizeof( buffer ), "%.*f", static_cast<int>( fractionDigits ), magnitude );
            std::string text = ( written > 0 && static_cast<size_t>( written ) < sizeof( buffer ) )
                                    ? std::string( buffer, static_cast<size_t>( written ) )
                                    : std::string( "0" );

            DigitRun     run;
            const size_t point = text.find( '.' );
            run.IntegerLen     = point == std::string::npos ? text.size() : point;
            run.Digits         = text;
            if ( point != std::string::npos )
            {
                run.Digits.erase( point, 1 );
            }
            return run;
        }

        // Group the integer digits every three from the right. THE SUBTRACTION IS DONE IN A SIGNED WIDTH
        // ON PURPOSE: the same grouping written as `( i - lead ) % 3 == 0` over size_t shipped in this
        // engine's Details panel and wrapped for every i below `lead`, putting a separator inside the
        // leading group ("1 2" for 12). Counting the digits that REMAIN avoids the subtraction entirely.
        std::string GroupInteger( const std::string_view digits, const std::string_view separator )
        {
            std::string out;
            out.reserve( digits.size() + digits.size() / 3 * separator.size() );
            for ( size_t i = 0; i < digits.size(); ++i )
            {
                const size_t remaining = digits.size() - i;
                if ( i != 0 && remaining % 3 == 0 )
                {
                    out.append( separator );
                }
                out.push_back( digits[i] );
            }
            return out;
        }

        std::string FormatMagnitude( const LocaleRow& locale, const double magnitude,
                                     const int32_t fractionDigits )
        {
            const int32_t  digits = fractionDigits < 0 ? 0 : fractionDigits;
            const DigitRun run    = RoundedDigits( magnitude, digits );

            std::string out =
                 GroupInteger( std::string_view( run.Digits ).substr( 0, run.IntegerLen ), locale.GroupSeparator );
            if ( run.Digits.size() > run.IntegerLen )
            {
                out.append( locale.DecimalSeparator );
                out.append( run.Digits, run.IntegerLen, std::string::npos );
            }
            return out;
        }
    } // namespace

    std::string FormatNumber( const LocaleRow& locale, const double value, const int32_t fractionDigits )
    {
        if ( !std::isfinite( value ) )
        {
            // NOT an empty string and not "0": a non-finite number reaching a label is a defect upstream,
            // and the label is the only place it can announce itself.
            return std::isnan( value ) ? std::string( "NaN" ) : std::string( value < 0 ? "-Inf" : "Inf" );
        }

        // The sign is taken from the VALUE and not from the printed digits: -0.4 shown with no decimals
        // rounds to "0", and "-0" is not a number anybody meant to display.
        const std::string magnitude = FormatMagnitude( locale, std::fabs( value ), fractionDigits );
        const bool        negative  = value < 0.0 && magnitude.find_first_of( "123456789" ) != std::string::npos;
        return negative ? "-" + magnitude : magnitude;
    }

    std::string FormatInteger( const LocaleRow& locale, const int64_t value )
    {
        // Through the digit path rather than through a double, because a double cannot hold every int64
        // exactly and a player's money is exactly the number that goes past 2^53.
        const bool    negative = value < 0;
        std::string   digits;
        std::uint64_t magnitude =
             negative ? ( ~static_cast<std::uint64_t>( value ) + 1u ) : static_cast<std::uint64_t>( value );
        do
        {
            digits.insert( digits.begin(), static_cast<char>( '0' + magnitude % 10 ) );
            magnitude /= 10;
        } while ( magnitude != 0 );

        const std::string grouped = GroupInteger( digits, locale.GroupSeparator );
        return negative ? "-" + grouped : grouped;
    }

    std::string FormatCurrency( const LocaleRow& locale, const double amount, const CurrencyRow& currency )
    {
        const std::string number = FormatNumber( locale, amount, currency.FractionDigits );

        // The pattern's only job here is to say where the symbol goes and what sits between it and the
        // number. The numeric skeleton inside it ("#,##0.00") has already been honoured by FormatNumber
        // through the locale's own separators and the currency's own digit count, so it is dropped rather
        // than interpreted twice — two implementations of one layout is the drift this engine keeps paying
        // for elsewhere.
        std::string out;
        out.reserve( locale.CurrencyPattern.size() + number.size() + currency.Symbol.size() );

        const std::string_view sign          = "\xC2\xA4"; // U+00A4 CURRENCY SIGN
        const std::string_view pattern       = locale.CurrencyPattern;
        bool                   numberWritten = false;
        for ( size_t i = 0; i < pattern.size(); )
        {
            if ( pattern.compare( i, sign.size(), sign ) == 0 )
            {
                out.append( currency.Symbol );
                i += sign.size();
                continue;
            }
            if ( pattern[i] == '#' || pattern[i] == '0' || pattern[i] == ',' || pattern[i] == '.' )
            {
                // The first character of the numeric skeleton: emit the number once and skip the rest.
                // A FLAG rather than "has the number been written already?", because the currency symbol
                // can legitimately contain the same characters the number does and a search would then
                // drop the amount.
                if ( !numberWritten )
                {
                    out.append( number );
                    numberWritten = true;
                }
                ++i;
                continue;
            }
            out.push_back( pattern[i] );
            ++i;
        }
        return out;
    }

    Common::ResultStr<std::string> FormatDate( const LocaleRow& locale, const CalendarDate& date )
    {
        std::string            out;
        const std::string_view pattern = locale.DatePattern;

        auto appendPadded = [&out]( const int32_t value, const size_t width )
        {
            std::string digits = std::to_string( value < 0 ? -value : value );
            if ( value < 0 )
            {
                digits.insert( digits.begin(), '-' );
            }
            while ( digits.size() < width )
            {
                digits.insert( digits.begin(), '0' );
            }
            out.append( digits );
        };

        for ( size_t i = 0; i < pattern.size(); )
        {
            const char letter = pattern[i];

            if ( letter == '\'' )
            {
                // CLDR quoting: '' is a literal apostrophe, '...' is a literal run. Supported because the
                // patterns are DATA and several CLDR rows outside this build's seven use it.
                ++i;
                if ( i < pattern.size() && pattern[i] == '\'' )
                {
                    out.push_back( '\'' );
                    ++i;
                    continue;
                }
                while ( i < pattern.size() && pattern[i] != '\'' )
                {
                    out.push_back( pattern[i++] );
                }
                if ( i < pattern.size() )
                {
                    ++i; // the closing quote
                }
                continue;
            }

            if ( ( letter >= 'a' && letter <= 'z' ) || ( letter >= 'A' && letter <= 'Z' ) )
            {
                size_t run = 0;
                while ( i + run < pattern.size() && pattern[i + run] == letter )
                {
                    ++run;
                }

                switch ( letter )
                {
                    case 'd':
                        appendPadded( date.Day, run );
                        break;
                    case 'M':
                        // `MMM` and longer ask for a NAME. This build has no month names (see the header),
                        // and printing the number instead would be a wrong answer wearing a right one's
                        // clothes.
                        if ( run >= 3 )
                            return Common::MakeFormattedError<std::string>(
                                 "locale '{}' has date pattern '{}', whose '{}' asks for a month NAME; this "
                                 "build carries numeric date fields only",
                                 locale.Tag, pattern, std::string( run, 'M' ) );
                        appendPadded( date.Month, run );
                        break;
                    case 'y':
                        // `yy` is the two-digit year, every other width is the full year zero-padded.
                        if ( run == 2 )
                        {
                            appendPadded( ( ( date.Year % 100 ) + 100 ) % 100, 2 );
                        }
                        else
                        {
                            appendPadded( date.Year, run );
                        }
                        break;
                    default:
                        return Common::MakeFormattedError<std::string>(
                             "locale '{}' has date pattern '{}', whose field '{}' this build cannot format "
                             "(only d, M and y are numeric)",
                             locale.Tag, pattern, std::string( run, letter ) );
                }
                i += run;
                continue;
            }

            out.push_back( letter );
            ++i;
        }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Localization
