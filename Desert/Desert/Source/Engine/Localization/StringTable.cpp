#include "StringTable.hpp"

#include <Engine/Localization/LocaleFormat.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <unordered_set>

namespace Desert::Localization
{
    std::optional<Gender> GenderFromName( const std::string_view name )
    {
        for ( uint8_t i = 1; i <= static_cast<uint8_t>( Gender::Common ); ++i )
        {
            const auto gender = static_cast<Gender>( i );
            if ( GenderName( gender ) == name )
            {
                return gender;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> ValidFormSelectors()
    {
        std::vector<std::string> out;
        for ( uint8_t p = 0; p <= static_cast<uint8_t>( PluralCategory::Other ); ++p )
            out.emplace_back( PluralCategoryName( static_cast<PluralCategory>( p ) ) );
        for ( uint8_t g = 1; g <= static_cast<uint8_t>( Gender::Common ); ++g )
        {
            const std::string gender{ GenderName( static_cast<Gender>( g ) ) };
            out.push_back( gender );
            for ( uint8_t p = 0; p <= static_cast<uint8_t>( PluralCategory::Other ); ++p )
                out.push_back( gender + "." +
                               std::string( PluralCategoryName( static_cast<PluralCategory>( p ) ) ) );
        }
        return out;
    }

    namespace
    {
        // A key is what an author TYPES into a text field after the `#`, so its alphabet is the one that
        // survives being typed, grepped and put in a URL. The restriction is not decoration: it is what
        // lets the resolver treat "the rest of the string after #" as the whole key with no escaping, and
        // what lets the hardcoded-text census recognise a key on sight.
        Common::BoolResultStr ValidateKey( const std::string& key )
        {
            if ( key.empty() )
                return Common::MakeFormattedError<bool>( "an entry has an empty Key" );
            for ( const char c : key )
            {
                const bool ok =
                     ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '.' || c == '_' || c == '-';
                if ( !ok )
                    return Common::MakeFormattedError<bool>(
                         "key '{}' contains '{}'; a key is lower-case letters, digits, '.', '_' and '-'", key,
                         std::string( 1, c ) );
            }
            return BOOLSUCCESS;
        }

        Common::BoolResultStr ValidateSelector( const std::string& key, const std::string& language,
                                                const std::string& selector )
        {
            const size_t dot = selector.find( '.' );
            if ( dot == std::string::npos )
            {
                if ( PluralCategoryFromName( selector ).has_value() || GenderFromName( selector ).has_value() )
                    return BOOLSUCCESS;
            }
            else
            {
                const bool gender = GenderFromName( selector.substr( 0, dot ) ).has_value();
                const bool plural = PluralCategoryFromName( selector.substr( dot + 1 ) ).has_value();
                if ( gender && plural )
                    return BOOLSUCCESS;
            }

            std::string allowed;
            for ( const std::string& name : ValidFormSelectors() )
            {
                if ( !allowed.empty() )
                    allowed += ", ";
                allowed += name;
            }
            return Common::MakeFormattedError<bool>(
                 "key '{}' language '{}' has form '{}', which is not a form selector; the grammar is: {}", key,
                 language, selector, allowed );
        }

        Common::BoolResultStr ValidateTable( const StringTableData& data )
        {
            std::unordered_set<std::string> seen;
            for ( const LocalizedEntry& entry : data.Entries )
            {
                if ( auto valid = ValidateKey( entry.Key ); !valid )
                    return valid;
                if ( !seen.insert( entry.Key ).second )
                    return Common::MakeFormattedError<bool>(
                         "key '{}' appears twice; a key resolves to one string, so a table cannot hold two",
                         entry.Key );

                if ( entry.Forms.empty() )
                    return Common::MakeFormattedError<bool>(
                         "key '{}' has no languages at all, so nothing can ever resolve it", entry.Key );

                for ( const auto& [language, forms] : entry.Forms )
                {
                    // AN UNKNOWN LANGUAGE IS A REFUSAL, not a row nobody will ever select. A table
                    // carrying "gb" instead of "en" would otherwise load, look complete, and show the key
                    // on every screen — with the one file that could explain it reporting success.
                    if ( FindLocale( language ) == nullptr )
                        return Common::MakeFormattedError<bool>(
                             "key '{}' has a translation for language '{}', which this build does not know; "
                             "the languages are listed in Engine/Localization/LocaleFormat.cpp",
                             entry.Key, language );

                    if ( forms.empty() )
                        return Common::MakeFormattedError<bool>(
                             "key '{}' declares language '{}' and gives it no forms", entry.Key, language );

                    // EVERY LANGUAGE MUST HAVE AN `other`, and this is the rule that makes a lookup
                    // total. `other` is the one category every language in CLDR has, and it is what the
                    // form ladder falls back to — so an entry without one is an entry that resolves to
                    // NOTHING for some input, and which input depends on the language:
                    //
                    //   * a Russian entry with only one/few/many breaks on a fraction ("1,5 файла"),
                    //     because `other` is exactly the category a printed fraction selects in Russian;
                    //   * a gendered entry with only masculine/feminine breaks for any caller that does
                    //     not know the gender, which is most callers.
                    //
                    // Refusing here rather than at the draw is the whole point: the author is looking at
                    // the file, and the alternative is a label that is fine for 99 counts and blank for
                    // the hundredth.
                    if ( forms.count( std::string( PluralCategoryName( PluralCategory::Other ) ) ) == 0 )
                        return Common::MakeFormattedError<bool>(
                             "key '{}' language '{}' has no 'other' form. Every language has that "
                             "category and the form ladder ends there, so an entry without one resolves to "
                             "nothing for some count or some gender",
                             entry.Key, language );

                    for ( const auto& [selector, text] : forms )
                    {
                        if ( auto valid = ValidateSelector( entry.Key, language, selector ); !valid )
                            return valid;
                        if ( text.empty() )
                            return Common::MakeFormattedError<bool>(
                                 "key '{}' language '{}' form '{}' is an EMPTY string; a blank label on "
                                 "screen is the least diagnosable thing this format can produce, so it is "
                                 "refused here instead",
                                 entry.Key, language, selector );
                    }
                }
            }
            return BOOLSUCCESS;
        }
    } // namespace

    Common::ResultStr<StringTableData> ParseStringTable( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<StringTableData>( "the file is empty" );

        // THE VERSION IS READ FIRST, ON ITS OWN, for the same reason CloudTypeData does it: a full parse
        // of a file from another format generation fails by naming a missing field, which is true and
        // useless — what the reader needs to be told is that the FORMAT moved. Read as an untyped tree,
        // because a struct would impose the very schema whose applicability is in question.
        if ( const auto tree = rfl::json::read<rfl::Generic>( text ); tree )
        {
            if ( const auto fields = tree.value().to_object(); fields )
            {
                if ( const auto stated = fields.value().get( "FormatVersion" ); stated.has_value() )
                {
                    const auto number = stated.value().to_int();
                    if ( number.has_value() && number.value() != kStringTableFormatVersion )
                        return Common::MakeFormattedError<StringTableData>(
                             "format version {} was written by a different build; this one reads version {}",
                             number.value(), kStringTableFormatVersion );
                }
            }
        }

        const auto parsed = rfl::json::read<StringTableData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<StringTableData>( "{}", parsed.error().what() );

        StringTableData data = parsed.value();

        const int32_t version = data.FormatVersion.value_or( kStringTableFormatVersion );
        if ( version != kStringTableFormatVersion )
            return Common::MakeFormattedError<StringTableData>(
                 "format version {} was written by a different build; this one reads version {}", version,
                 kStringTableFormatVersion );

        if ( auto valid = ValidateTable( data ); !valid )
            return Common::MakeFormattedError<StringTableData>( "{}", valid.GetError() );

        data.FormatVersion = kStringTableFormatVersion;
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteStringTable( const StringTableData& data )
    {
        StringTableData stamped = data;
        stamped.FormatVersion   = kStringTableFormatVersion;
        return rfl::json::write( stamped, YYJSON_WRITE_PRETTY );
    }
} // namespace Desert::Localization
