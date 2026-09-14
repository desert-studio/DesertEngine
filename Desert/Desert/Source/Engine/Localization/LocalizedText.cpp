#include "LocalizedText.hpp"

#include <algorithm>
#include <cstdlib>

namespace Desert::Localization
{
    bool IsKeyReference( const std::string_view authored )
    {
        // Exactly one leading sigil. `"##x"` is the escape and therefore a literal; `"#"` on its own is a
        // key reference to the empty key, which the resolver names in its refusal.
        return !authored.empty() && authored.front() == kKeySigil &&
               ( authored.size() == 1 || authored[1] != kKeySigil );
    }

    std::string_view KeyOf( const std::string_view authored )
    {
        return IsKeyReference( authored ) ? authored.substr( 1 ) : std::string_view{};
    }

    std::string LiteralOf( const std::string_view authored )
    {
        // Only the LEADING pair is an escape. A hash anywhere else in a literal is just a hash — a colour
        // in rich text ("[color=#FF7A33]") is the case that makes this non-negotiable, and doubling every
        // hash in the string would have broken every one of them.
        if ( authored.size() >= 2 && authored[0] == kKeySigil && authored[1] == kKeySigil )
            return std::string( authored.substr( 1 ) );
        return std::string( authored );
    }

    std::string EscapeLiteral( const std::string_view literal )
    {
        if ( !literal.empty() && literal.front() == kKeySigil )
            return std::string( 1, kKeySigil ) + std::string( literal );
        return std::string( literal );
    }

    const std::string* SelectForm( const std::map<std::string, std::string>& forms, const PluralCategory category,
                                   const Gender gender )
    {
        const std::string plural{ PluralCategoryName( category ) };
        const std::string genderName{ GenderName( gender ) };

        auto lookup = [&forms]( const std::string& selector ) -> const std::string*
        {
            const auto it = forms.find( selector );
            return it == forms.end() ? nullptr : &it->second;
        };

        // The ladder, most specific first. It is an ordered list and not a search, so a table that
        // happens to contain an unrelated form can never answer a question it was not written for.
        if ( !genderName.empty() )
        {
            if ( const std::string* both = lookup( genderName + "." + plural ) )
            {
                return both;
            }
            if ( const std::string* onlyGender = lookup( genderName ) )
            {
                return onlyGender;
            }
        }
        if ( const std::string* onlyPlural = lookup( plural ) )
        {
            return onlyPlural;
        }
        return lookup( std::string( PluralCategoryName( PluralCategory::Other ) ) );
    }

    FormattedText ApplyArguments( const std::string_view pattern, const LocaleRow& locale,
                                  const FormatArguments& args )
    {
        FormattedText out;
        out.Text.reserve( pattern.size() );

        for ( size_t i = 0; i < pattern.size(); )
        {
            if ( pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '{' )
            {
                out.Text.push_back( '{' );
                i += 2;
                continue;
            }
            if ( pattern[i] == '}' && i + 1 < pattern.size() && pattern[i + 1] == '}' )
            {
                out.Text.push_back( '}' );
                i += 2;
                continue;
            }
            if ( pattern[i] != '{' )
            {
                out.Text.push_back( pattern[i] );
                ++i;
                continue;
            }

            const size_t close = pattern.find( '}', i + 1 );
            if ( close == std::string_view::npos )
            {
                // An unterminated brace is text, not a placeholder. Swallowing the rest of the string
                // would delete a translation because somebody typed one character.
                out.Text.append( pattern.substr( i ) );
                break;
            }

            const std::string_view body  = pattern.substr( i + 1, close - i - 1 );
            const size_t           colon = body.find( ':' );
            const std::string_view name  = body.substr( 0, colon == std::string_view::npos ? body.size() : colon );

            if ( name == "n" )
            {
                if ( args.Count.has_value() )
                {
                    int32_t digits = args.CountFractionDigits;
                    if ( colon != std::string_view::npos )
                    {
                        const std::string spec( body.substr( colon + 1 ) );
                        digits = static_cast<int32_t>( std::strtol( spec.c_str(), nullptr, 10 ) );
                    }
                    out.Text.append( FormatNumber( locale, *args.Count, digits ) );
                    i = close + 1;
                    continue;
                }
            }
            else
            {
                const auto named = std::find_if( args.Named.begin(), args.Named.end(),
                                                 [&name]( const auto& pair ) { return pair.first == name; } );
                if ( named != args.Named.end() )
                {
                    out.Text.append( named->second );
                    i = close + 1;
                    continue;
                }
            }

            // Nothing answered for it. The placeholder stays on screen exactly as written — visible,
            // non-empty, and obviously not a translation — and its name goes back to the caller to log.
            out.Text.append( pattern.substr( i, close - i + 1 ) );
            out.Unresolved.emplace_back( name );
            i = close + 1;
        }

        return out;
    }
} // namespace Desert::Localization
