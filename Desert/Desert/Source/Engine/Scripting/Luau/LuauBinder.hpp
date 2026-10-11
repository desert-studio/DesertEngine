#pragma once

// INTERNAL to Engine/Scripting/Luau: the only header besides the runtime's .cpp that sees Luau's types.

#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>
#include <Engine/Scripting/Luau/LuauRuntime.hpp>

#include <optional>
#include <string>

#include <VM/include/lua.h>

namespace Desert::Scripting::LuauBinder
{
    /// THE ONE BINDER: reflection -> Luau, generic over every type. It turns a TypeInfo's fields and its
    /// FUNCTION(ScriptCallable) members into script access, converting through Reflection::Value only — the
    /// conversions below are the language rule (a Luau number is the Float a parameter wants), the call and
    /// the field write are the reflection layer's (FunctionInfo::Invoke, WriteField).
    ///
    /// Installs the object metatable, the object destructor and one global table per reflected type that
    /// has static ScriptCallable functions (`Type.Name(...)`). Runs on the main state BEFORE luaL_sandbox, so
    /// what it creates is frozen with the rest.
    void Install( lua_State* L );

    /// The registry table of the engine's entity methods (destroy, call, move, ...): the entity object's
    /// __index looks a name up here after its own reflection methods (component/has/add/remove).
    inline constexpr const char* kEntityMethods = "desert.entity.methods";

    /// Adds `entity:name(...)`, a native the host module installs (on the main state, before the sandbox).
    void SetEntityMethod( lua_State* L, const char* name, lua_CFunction method );

    /// The entity an object at `index` binds, or nullopt when it is not an entity object.
    std::optional<LuauEntityRef> ToEntity( lua_State* L, int index );

    /// The LIVE entity at `index`, or a Luau error (not an entity / gone).
    LuauEntityRef CheckEntity( lua_State* L, int index );

    /// Pushes `entity` as an entity object (nil when null or destroyed).
    void PushEntity( lua_State* L, entt::registry& registry, entt::entity entity );

    /// Pushes `binding` as an object: tagged userdata owning a copy of the binding (destroyed by the VM).
    void PushObject( lua_State* L, const LuauBinding& binding );

    /// Pushes a Value in its Luau form: number, boolean, string, vector (Vec2 has z = 0), {x,y,z,w} for Vec4.
    void PushValue( lua_State* L, const Reflection::Value& value );

    /// The value at `index` as a Value of kind `expected`, or nullopt with `why` set.
    std::optional<Reflection::Value> ToValue( lua_State* L, int index, Reflection::FieldType expected,
                                              std::string& why );
} // namespace Desert::Scripting::LuauBinder
