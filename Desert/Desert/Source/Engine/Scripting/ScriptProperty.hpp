#pragma once

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Scripting
{
    // THE KINDS OF A SCRIPT PROPERTY — what a script declares in its `Properties` table (Property.Float(5, {...}),
    // Property.Entity(), ...; a plain literal is refused at load) and what Details draws a widget for.
    // The numbers are stored in scenes: append, never renumber.
    enum class PropertyType
    {
        Number = 0, // a Luau number (Property.Float)
        Bool   = 1,
        String = 2,
        Int    = 3, // Property.Int: a whole number (Number holds it)
        Color  = 4, // Property.Color: linear RGB in Vec, a Luau vector in the script
        Vector = 5, // Property.Vector: Vec (centimetres for a position), a Luau vector in the script
        Asset  = 6, // Property.Asset: Ref holds the asset's handle (0 = none); the script reads the handle,
                    // which it can hand to any asset field (mesh.Mesh = Properties.Crate)
        Entity = 7, // Property.Entity: Ref holds the referenced entity's UUID (0 = none); the script reads
                    // the entity object, or nil
        Enum  = 8,  // Property.Enum: Str holds the enumerator's name; the script reads that name
        Array = 9,  // Property.Array: Items, each of ElementType
    };

    // ONE VALUE of a script property — a scalar (an Array's element is one of these). POD only (NO Luau types),
    // so it can live in ScriptComponent / Components.hpp without leaking the scripting backend.
    struct ScriptValue
    {
        PropertyType          Type   = PropertyType::Number;
        double                Number = 0.0;   // Number, Int
        bool                  Bool   = false; // Bool
        std::string           Str;            // String, Enum (enumerator name)
        std::array<double, 3> Vec{};          // Color (linear RGB), Vector
        std::uint64_t         Ref = 0;        // Asset: its handle; Entity: its UUID; 0 = none

        bool operator==( const ScriptValue& ) const = default;
    };

    // A single editor-exposed script variable's VALUE on one entity. The script declares the schema (names, kinds,
    // defaults, metadata: ScriptSchema.hpp); a slot holds only the values the user OVERRODE in Details — like a UE
    // instance's delta from its class default — so a script's new default reaches every entity that never touched
    // the property. The values are written into the script's environment before it runs.
    struct ScriptProperty : ScriptValue
    {
        std::string              Name;
        PropertyType             ElementType = PropertyType::Number; // Array only: the kind of every item
        std::vector<ScriptValue> Items;                              // Array only

        bool operator==( const ScriptProperty& ) const = default;
    };

    // The spelling of a kind in a diagnostic and in a save game ("Number", "Color", ...).
    [[nodiscard]] const char* PropertyTypeName( PropertyType type );

    // The names a script marks SaveGame - UE's SaveGame flag on a Blueprint variable. Declared in the script
    // beside its `Properties` table, by name:
    //
    //     Properties         = { Score = 0, Coins = 0, WalkSpeed = 300 }
    //     SaveGameProperties = { "Score", "Coins" }
    //
    // A SaveGame property is game state, not tuning: what the running script writes to `Properties.<name>` is
    // read back into its slot (ScriptEngine::ReadBackProperties) and a save game stores it per entity UUID +
    // script + property name (Core/SaveGame.hpp). Read from the FILE, never stored in the scene: the script is
    // the one declaration. A script without the list marks none. Refused by name: an unreadable script, a
    // `SaveGameProperties` that is not a list of strings, a name its `Properties` does not declare.
    [[nodiscard]] Common::ResultStr<std::vector<std::string>>
    ReadScriptSaveGameProperties( const std::string& path );
} // namespace Desert::Scripting
