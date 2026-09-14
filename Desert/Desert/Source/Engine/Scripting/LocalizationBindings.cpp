#include "Internal/ScriptRuntime.hpp"

#include <Engine/Localization/LocalizationService.hpp>

namespace Desert::Scripting
{
    // Localisation, from the script side. This is where GENDER enters the engine, and it is the reason
    // this table exists rather than everything going through an authored `#key` in a component:
    //
    //   loc.text( "menu.play" )                            -- the translation, in the current language
    //   loc.plural( "files.count", 5 )                     -- ... with a count; the form follows CLDR
    //   loc.text( "joined", { gender = "feminine" } )      -- ... and a gender the CALLER knows
    //   loc.number( 1234.5, 2 ) / loc.money( 99, "EUR" ) / loc.date( 2026, 9, 14 )
    //   loc.language() / loc.set_language( "ru" ) / loc.languages()
    //
    // A key's plural form is a property of the NUMBER and a gender is a property of the SUBJECT, and only
    // gameplay knows the subject — so it is passed here and never guessed from a data-store key. The
    // result usually goes straight into `ui.set`, which is how a translated, counted, gendered sentence
    // reaches a label without the canvas knowing any of it.
    void RegisterLocalizationBindings( ScriptEngine::Impl& implRef )
    {
        auto& lua = implRef.Lua;

        sol::table loc = lua.create_named_table( "loc" );

        // Reads the optional argument table of `text`/`plural`. Unknown keys are IGNORED rather than
        // refused, and that is the one place here where silence is right: Lua tables are open, and an
        // engine that errored on a stray field would make every call site fragile. What is NOT ignored is
        // a gender NAME it does not know — that is a typo in a fixed vocabulary and it says so.
        auto readArgs = []( const sol::optional<sol::table>& options )
        {
            Localization::FormatArguments args;
            if ( !options )
                return args;

            const sol::table& table = *options;
            if ( const sol::optional<double> count = table["count"] )
                args.Count = *count;
            if ( const sol::optional<int> digits = table["digits"] )
                args.CountFractionDigits = *digits;
            if ( const sol::optional<std::string> gender = table["gender"] )
            {
                if ( const auto parsed = Localization::GenderFromName( *gender ) )
                {
                    args.Subject = *parsed;
                }
                else
                {
                    LOG_ERROR( "[Localization] script passed gender '{}', which is not one of masculine, "
                               "feminine, neuter, common — the string will be resolved without a gender",
                               *gender );
                }
            }
            // Everything else in the table is a NAMED placeholder, already turned into text by whoever
            // knows what it is. Numbers are formatted for the current locale on the way in, so a script
            // cannot accidentally put a C-locale "1234.5" into a French sentence.
            for ( const auto& [key, value] : table )
            {
                const sol::optional<std::string> name = key.as<sol::optional<std::string>>();
                if ( !name || *name == "count" || *name == "digits" || *name == "gender" )
                    continue;
                if ( value.is<std::string>() )
                    args.Named.emplace_back( *name, value.as<std::string>() );
                else if ( value.is<double>() )
                    args.Named.emplace_back(
                         *name, Localization::FormatNumber( Localization::Localization::Get().Language(),
                                                            value.as<double>(), 0 ) );
            }
            return args;
        };

        loc.set_function( "text", [readArgs]( const std::string& key, const sol::optional<sol::table>& options )
                          { return Localization::Localization::Get().Format( key, readArgs( options ) ).Text; } );

        // The common case, spelled so it cannot be got wrong: a key and a number.
        loc.set_function(
             "plural",
             [readArgs]( const std::string& key, double count, const sol::optional<sol::table>& options )
             {
                 Localization::FormatArguments args = readArgs( options );
                 args.Count                         = count;
                 return Localization::Localization::Get().Format( key, args ).Text;
             } );

        loc.set_function( "number",
                          []( double value, const sol::optional<int>& digits )
                          {
                              return Localization::FormatNumber( Localization::Localization::Get().Language(),
                                                                 value, digits ? *digits : 0 );
                          } );

        loc.set_function( "money",
                          []( double amount, const std::string& code ) -> std::string
                          {
                              const Localization::CurrencyRow* currency = Localization::FindCurrency( code );
                              if ( currency == nullptr )
                              {
                                  // The CODE is returned beside the raw amount rather than a plausible-looking sum
                                  // in the wrong currency. A price shown in the wrong money is the one formatting
                                  // mistake a player will act on.
                                  LOG_ERROR( "[Localization] currency '{}' is not one this build knows", code );
                                  return Localization::FormatNumber( Localization::Localization::Get().Language(),
                                                                     amount, 2 ) +
                                         " " + code;
                              }
                              return Localization::FormatCurrency( Localization::Localization::Get().Language(),
                                                                   amount, *currency );
                          } );

        loc.set_function( "date",
                          []( int year, int month, int day ) -> std::string
                          {
                              const auto formatted =
                                   Localization::FormatDate( Localization::Localization::Get().Language(),
                                                             Localization::CalendarDate{ year, month, day } );
                              if ( !formatted )
                              {
                                  LOG_ERROR( "[Localization] {}", formatted.GetError() );
                                  return {};
                              }
                              return formatted.GetValue();
                          } );

        loc.set_function( "language",
                          [] { return std::string( Localization::Localization::Get().Language().Tag ); } );

        // Returns false AND logs, rather than raising: a language a project does not ship is a content
        // problem, and a script that asks for one should be able to fall back rather than die.
        loc.set_function( "set_language",
                          []( const std::string& tag )
                          {
                              const auto set = Localization::Localization::Get().SetLanguage( tag );
                              if ( !set )
                                  LOG_ERROR( "[Localization] {}", set.GetError() );
                              return set.IsSuccess();
                          } );

        // The languages this PROJECT has strings in — derived from the loaded tables, not declared
        // anywhere — which is what a settings screen should offer.
        loc.set_function( "languages",
                          [&lua]
                          {
                              sol::table out = lua.create_table();
                              int        i   = 1;
                              for ( const Localization::LocaleRow* row :
                                    Localization::Localization::Get().AvailableLanguages() )
                              {
                                  sol::table entry = lua.create_table();
                                  entry["tag"]     = std::string( row->Tag );
                                  entry["name"]    = std::string( row->Endonym );
                                  out[i++]         = entry;
                              }
                              return out;
                          } );
    }
} // namespace Desert::Scripting
