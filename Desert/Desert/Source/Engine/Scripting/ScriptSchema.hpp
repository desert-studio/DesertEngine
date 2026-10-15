#pragma once

#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <string>
#include <vector>

namespace Desert::Scripting
{
    // ONE DECLARED PROPERTY OF A SCRIPT — the script-side twin of a reflected C++ field (Reflection::FieldInfo):
    // its kind, its default, and the SAME metadata vocabulary a PROPERTY(...) carries
    // (Reflection::PropertyMetadata: DisplayName, Category, Tooltip, Range, EditCondition, Units, Advanced,
    // AssetType), so Details draws a script variable with the rules it draws an engine field with. Declared in the
    // script:
    //
    //     Properties = {
    //         Speed  = Property.Float( 300, { min = 0, max = 1200, units = "cm/s", category = "Movement",
    //                                         tooltip = "How fast the pawn walks." } ),
    //         Target = Property.Entity( { tooltip = "Who to follow." } ),
    //         Mode   = Property.Enum( { "Idle", "Patrol", "Chase" }, "Patrol" ),
    //         Points = Property.Array( Property.Vector( 0, 0, 0 ), {} ),
    //         Lives  = 3, -- a plain literal: a Number / Bool / String with no metadata
    //     }
    //
    // Language-free: the Luau plugin builds these (Luau/LuauProperties.cpp); the editor and the runtime read them.
    struct ScriptPropertyDecl
    {
        std::string                  Name;
        ScriptProperty               Default; // Name, kind and the default value (Items for an Array)
        Reflection::PropertyMetadata Meta;
        std::vector<std::string>     EnumOptions; // Enum (or an Array of Enum): the enumerators, in order
        PropertyType                 ElementType = PropertyType::Number; // Array: the kind of its items
        Reflection::PropertyMetadata ElementMeta;                        // Array: the items' range, units, ...
    };

    // A function the script offers as a BUTTON in Details — UE's UFUNCTION(CallInEditor). Declared by name:
    //
    //     CallInEditor = { "Respawn", "ShufflePath" }
    //
    // The editor runs it on the edited entity in the edit world, with the slot's property values applied.
    struct ScriptEditorFunction
    {
        std::string Name;
    };

    // Where a script stands when the editor reads it.
    enum class ScriptStatus
    {
        Ok,       // compiled, its top level ran, its declarations read
        NotFound, // no file at the slot's path
        Error,    // a compile error, a top-level runtime error or a malformed declaration
    };

    // THE SCRIPT AS THE EDITOR SEES IT — UE's class default object + its UPROPERTY list, for one script file.
    struct ScriptSchema
    {
        ScriptStatus Status = ScriptStatus::Ok;
        std::string  Error;         // Status != Ok: the message, as the language reported it
        int          ErrorLine = 0; // the 1-based line the error names, 0 when it names none

        std::string                       Description; // the comment block the script opens with
        std::vector<ScriptPropertyDecl>   Properties;  // in declaration order
        std::vector<ScriptEditorFunction> EditorFunctions;
    };

    // Reads a script's schema WITHOUT running the game: the file's top level runs in a throwaway runtime with no
    // engine modules. The editor caches the result per file and modification time (it is not free).
    [[nodiscard]] ScriptSchema ReadScriptSchema( const std::string& path );

    // The line an error message of the scripting language names (`<chunk>:<line>: message`), or 0.
    [[nodiscard]] int ScriptErrorLine( const std::string& error );

    // The declaration named `name`, or nullptr.
    [[nodiscard]] const ScriptPropertyDecl* FindDecl( const ScriptSchema& schema, const std::string& name );
} // namespace Desert::Scripting
