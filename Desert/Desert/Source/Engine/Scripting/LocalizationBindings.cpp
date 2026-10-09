#include "Internal/ScriptRuntime.hpp"

#include <Engine/Localization/LocalizationService.hpp>

namespace Desert::Scripting
{
    namespace
    {
        const Localization::LocaleRow& Language()
        {
            return Localization::Localization::Get().Language();
        }

        // Reads the optional argument table of `text`/`plural` at `index`. Unknown keys are IGNORED rather than
        // refused, and that is the one place here where silence is right: Lua tables are open, and an
        // engine that errored on a stray field would make every call site fragile. What is NOT ignored is
        // a gender NAME it does not know — that is a typo in a fixed vocabulary and it says so.
        Localization::FormatArguments ReadArgs( lua_State* L, int index )
        {
            Localization::FormatArguments args;
            if ( lua_isnoneornil( L, index ) )
                return args;
            luaL_checktype( L, index, LUA_TTABLE );

            lua_pushnil( L );
            while ( lua_next( L, index ) != 0 )
            {
                if ( lua_type( L, -2 ) == LUA_TSTRING )
                {
                    const std::string name = lua_tostring( L, -2 );
                    const int         type = lua_type( L, -1 );
                    if ( name == "count" && type == LUA_TNUMBER )
                    {
                        args.Count = lua_tonumber( L, -1 );
                    }
                    else if ( name == "digits" && type == LUA_TNUMBER )
                    {
                        args.CountFractionDigits = static_cast<int>( lua_tonumber( L, -1 ) );
                    }
                    else if ( name == "gender" && type == LUA_TSTRING )
                    {
                        const std::string gender = lua_tostring( L, -1 );
                        if ( const auto parsed = Localization::GenderFromName( gender ) )
                            args.Subject = *parsed;
                        else
                            LOG_ERROR( "[Localization] script passed gender '{}', which is not one of masculine, "
                                       "feminine, neuter, common — the string will be resolved without a gender",
                                       gender );
                    }
                    else if ( name != "count" && name != "digits" && name != "gender" )
                    {
                        // Everything else is a NAMED placeholder, already turned into text by whoever knows
                        // what it is. Numbers are formatted for the current locale on the way in, so a script
                        // cannot accidentally put a C-locale "1234.5" into a French sentence.
                        if ( type == LUA_TSTRING )
                            args.Named.emplace_back( name, lua_tostring( L, -1 ) );
                        else if ( type == LUA_TNUMBER )
                            args.Named.emplace_back(
                                 name, Localization::FormatNumber( Language(), lua_tonumber( L, -1 ), 0 ) );
                    }
                }
                lua_pop( L, 1 );
            }
            return args;
        }

        void PushText( lua_State* L, const std::string& text )
        {
            lua_pushlstring( L, text.data(), text.size() );
        }

        int Text( lua_State* L )
        {
            const std::string key = luaL_checkstring( L, 1 );
            PushText( L, Localization::Localization::Get().Format( key, ReadArgs( L, 2 ) ).Text );
            return 1;
        }

        // The common case, spelled so it cannot be got wrong: a key and a number.
        int Plural( lua_State* L )
        {
            const std::string             key   = luaL_checkstring( L, 1 );
            const double                  count = luaL_checknumber( L, 2 );
            Localization::FormatArguments args  = ReadArgs( L, 3 );
            args.Count                          = count;
            PushText( L, Localization::Localization::Get().Format( key, args ).Text );
            return 1;
        }

        int Number( lua_State* L )
        {
            const double value  = luaL_checknumber( L, 1 );
            const int    digits = static_cast<int>( luaL_optinteger( L, 2, 0 ) );
            PushText( L, Localization::FormatNumber( Language(), value, digits ) );
            return 1;
        }

        int Money( lua_State* L )
        {
            const double                     amount   = luaL_checknumber( L, 1 );
            const std::string                code     = luaL_checkstring( L, 2 );
            const Localization::CurrencyRow* currency = Localization::FindCurrency( code );
            if ( currency == nullptr )
            {
                // The CODE is returned beside the raw amount rather than a plausible-looking sum in the wrong
                // currency. A price shown in the wrong money is the one formatting mistake a player will act on.
                LOG_ERROR( "[Localization] currency '{}' is not one this build knows", code );
                PushText( L, Localization::FormatNumber( Language(), amount, 2 ) + " " + code );
                return 1;
            }
            PushText( L, Localization::FormatCurrency( Language(), amount, *currency ) );
            return 1;
        }

        int Date( lua_State* L )
        {
            const Localization::CalendarDate date{ static_cast<int>( luaL_checkinteger( L, 1 ) ),
                                                   static_cast<int>( luaL_checkinteger( L, 2 ) ),
                                                   static_cast<int>( luaL_checkinteger( L, 3 ) ) };
            const auto                       formatted = Localization::FormatDate( Language(), date );
            if ( !formatted )
            {
                LOG_ERROR( "[Localization] {}", formatted.GetError() );
                PushText( L, {} );
                return 1;
            }
            PushText( L, formatted.GetValue() );
            return 1;
        }

        int LanguageTag( lua_State* L )
        {
            PushText( L, std::string( Language().Tag ) );
            return 1;
        }

        // Returns false AND logs, rather than raising: a language a project does not ship is a content
        // problem, and a script that asks for one should be able to fall back rather than die.
        int SetLanguage( lua_State* L )
        {
            const auto set = Localization::Localization::Get().SetLanguage( luaL_checkstring( L, 1 ) );
            if ( !set )
                LOG_ERROR( "[Localization] {}", set.GetError() );
            lua_pushboolean( L, set.IsSuccess() ? 1 : 0 );
            return 1;
        }

        // The languages this PROJECT has strings in — derived from the loaded tables, not declared
        // anywhere — which is what a settings screen should offer.
        int Languages( lua_State* L )
        {
            lua_newtable( L );
            int i = 1;
            for ( const Localization::LocaleRow* row : Localization::Localization::Get().AvailableLanguages() )
            {
                lua_newtable( L );
                PushText( L, std::string( row->Tag ) );
                lua_setfield( L, -2, "tag" );
                PushText( L, std::string( row->Endonym ) );
                lua_setfield( L, -2, "name" );
                lua_rawseti( L, -2, i++ );
            }
            return 1;
        }
    } // namespace

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
    void RegisterLocalizationBindings( lua_State* L )
    {
        constexpr luaL_Reg kLoc[] = { { "text", &Text },
                                      { "plural", &Plural },
                                      { "number", &Number },
                                      { "money", &Money },
                                      { "date", &Date },
                                      { "language", &LanguageTag },
                                      { "set_language", &SetLanguage },
                                      { "languages", &Languages },
                                      { nullptr, nullptr } };
        luaL_register( L, "loc", kLoc );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
