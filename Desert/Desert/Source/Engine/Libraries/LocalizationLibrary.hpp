#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

#include <string>
#include <vector>

namespace Desert::Libraries
{
    /// One language the project has strings in: its BCP-47 tag and what its speakers call it.
    struct LanguageInfo
    {
        REFLECT( ScriptStruct )

        PROPERTY()
        std::string Tag;

        PROPERTY()
        std::string Name;
    };

    /// Localisation from the script side — and where GENDER enters the engine: a key's plural form is a property
    /// of the NUMBER and a gender a property of the SUBJECT, and only gameplay knows the subject, so it is passed
    /// here and never guessed from a data-store key. The result usually goes straight into ui.set.
    ///
    ///   loc.text( "menu.play" )                          -- the translation, in the current language
    ///   loc.plural( "files.count", 5 )                   -- ... with a count; the form follows CLDR
    ///   loc.text( "joined", { gender = "feminine" } )    -- ... and a gender the CALLER knows
    ///
    /// The argument record's `count`, `digits` and `gender` are read as such; every other field is a NAMED
    /// placeholder (a string, or a number formatted for the current locale). A gender name outside the fixed
    /// vocabulary is logged — a typo, not an open field.
    struct LocalizationLibrary
    {
        REFLECT( ScriptName( "loc" ) )

        FUNCTION( ScriptCallable, ScriptName( "text" ), Tooltip( "The translation of a key, in the current language." ) )
        static std::string Text( const std::string& key, const Reflection::Value::Map& args = {} );

        FUNCTION( ScriptCallable, ScriptName( "plural" ), Tooltip( "The translation of a key for a count." ) )
        static std::string Plural( const std::string& key, double count, const Reflection::Value::Map& args = {} );

        FUNCTION( ScriptCallable, ScriptName( "number" ), Tooltip( "A number in the current locale's form." ) )
        static std::string Number( double value, int digits = 0 );

        FUNCTION( ScriptCallable, ScriptName( "money" ),
                  Tooltip( "An amount in a currency (ISO 4217 code); an unknown code is logged and shown raw." ) )
        static std::string Money( double amount, const std::string& code );

        FUNCTION( ScriptCallable, ScriptName( "date" ), Tooltip( "A calendar date in the current locale's form." ) )
        static std::string Date( int year, int month, int day );

        FUNCTION( ScriptCallable, ScriptName( "language" ), Tooltip( "The current language's tag." ) )
        static std::string Language();

        FUNCTION( ScriptCallable, ScriptName( "set_language" ),
                  Tooltip( "Switches the language; false (logged) when the project has no strings in it." ) )
        static bool SetLanguage( const std::string& tag );

        FUNCTION( ScriptCallable, ScriptName( "languages" ),
                  Tooltip( "The languages the project has strings in (what a settings screen offers)." ) )
        static std::vector<LanguageInfo> Languages();
    };
} // namespace Desert::Libraries
