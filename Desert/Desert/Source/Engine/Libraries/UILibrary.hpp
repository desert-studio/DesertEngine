#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

#include <string>

namespace Desert::Libraries
{
    /// The UI <-> gameplay bridge from the script side: gameplay writes DATA under a key and every element bound
    /// to that key follows on the next frame (UIDataStore), so renaming or restyling a widget cannot break
    /// gameplay code. A value is a number, a boolean, a string or a colour (a vector r, g, b). Collections are the
    /// records a UIListView shows; their indices are 1-based, as a script counts. A mutation answers (ok, error):
    /// an index outside the collection is the script's bug and it is told so, with the numbers.
    struct UILibrary
    {
        REFLECT( ScriptName( "ui" ) )

        FUNCTION( ScriptCallable, ScriptName( "set" ),
                  Tooltip( "Writes a number, boolean, string or colour (vector r, g, b) under a key; another kind "
                           "is refused and logged." ) )
        static void Set( const std::string& key, const Reflection::Value& value );

        FUNCTION( ScriptCallable, ScriptName( "get" ), Tooltip( "The value under a key, or nil when unset." ) )
        static Reflection::Value Get( const std::string& key );

        FUNCTION( ScriptCallable, ScriptName( "has" ) )
        static bool Has( const std::string& key );

        FUNCTION( ScriptCallable, ScriptName( "erase" ), Tooltip( "Removes one key." ) )
        static void Erase( const std::string& key );

        FUNCTION( ScriptCallable, ScriptName( "clear" ), Tooltip( "Removes every key and collection." ) )
        static void Clear();

        FUNCTION( ScriptCallable, ScriptName( "send" ),
                  Tooltip( "Raises a UI message on the canvas's own channel (every OnUIMessage hears it)." ) )
        static void Send( const std::string& message );

        FUNCTION( ScriptCallable, ScriptName( "toast" ),
                  Tooltip( "Queues a notification on the named Toast overlay (an event: twice queues twice)." ) )
        static void Toast( const std::string& overlay, const std::string& text );

        FUNCTION( ScriptCallable, ScriptName( "list_add" ), Tooltip( "Appends a record { field = value, ... }." ) )
        static Common::BoolResultStr ListAdd( const std::string& key, const Reflection::Value::Map& record );

        FUNCTION( ScriptCallable, ScriptName( "list_insert" ),
                  Tooltip( "Inserts a record so that it becomes the index-th (1-based)." ) )
        static Common::BoolResultStr ListInsert( const std::string& key, int index,
                                                 const Reflection::Value::Map& record );

        FUNCTION( ScriptCallable, ScriptName( "list_remove" ), Tooltip( "Removes the index-th record (1-based)." ) )
        static Common::BoolResultStr ListRemove( const std::string& key, int index );

        FUNCTION( ScriptCallable, ScriptName( "list_set" ),
                  Tooltip( "Writes one field of the index-th record (1-based)." ) )
        static Common::BoolResultStr ListSet( const std::string& key, int index, const std::string& field,
                                              const Reflection::Value& value );

        FUNCTION( ScriptCallable, ScriptName( "list_clear" ) )
        static void ListClear( const std::string& key );

        FUNCTION( ScriptCallable, ScriptName( "list_count" ), Tooltip( "How many records; 0 for no collection." ) )
        static int ListCount( const std::string& key );
    };
} // namespace Desert::Libraries
