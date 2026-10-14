#pragma once

// INTERNAL to Engine/Scripting/Luau: a script's declared properties in Luau terms.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Scripting/Luau/LuauRuntime.hpp>
#include <Engine/Scripting/ScriptSchema.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace Desert::Scripting::LuauProperties
{
    /// The userdata tag of a property declaration (LuauBinder holds 1..3).
    inline constexpr int kPropertyTag = 4;

    /// Installs the global `Property` module (Float, Int, Bool, String, Color, Vector, Asset, Entity, Enum,
    /// Array) on the main state, before luaL_sandbox. Each constructor validates its arguments where the script
    /// calls it — a wrong option is an error naming the script's line — and returns a DECLARATION, which only
    /// means something as an entry of the script's `Properties` table.
    void Install( lua_State* L );

    /// After a slot's top level ran: reads its `Properties` table — declarations and plain boolean / number /
    /// string literals — in declaration order, and replaces every entry with its default value in the form the
    /// script reads (a number, a vector, an asset handle, nil for an unset entity, a table for an array). Refused
    /// for an entry that is neither, naming it.
    [[nodiscard]] Common::ResultStr<std::vector<ScriptPropertyDecl>> Settle( lua_State* thread );

    /// What a value needs from the world it is pushed into: the entity a UUID names.
    using World = LuauPropertyWorld;

    /// Pushes `value` (of kind value.Type) in the form the script reads.
    void PushValue( lua_State* L, const ScriptValue& value, const World& world );

    /// Pushes a whole property (an Array as a table of its items).
    void PushProperty( lua_State* L, const ScriptProperty& property, const World& world );

    /// The value at `index` as a scalar of kind `type` (Number, Int, Bool, String, Enum), or false when it is
    /// not one — what a SaveGame read-back copies.
    [[nodiscard]] bool ReadScalar( lua_State* L, int index, PropertyType type, ScriptValue& out );
} // namespace Desert::Scripting::LuauProperties
