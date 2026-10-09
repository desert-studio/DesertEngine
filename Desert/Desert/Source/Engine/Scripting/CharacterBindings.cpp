#include "Internal/ScriptRuntime.hpp"

namespace Desert::Scripting
{
    // Character-controller & swimming API — GAMEPLAY domain. Lives apart from the script core:
    // extends the base Entity usertype; the engine's scripting core knows nothing about it.
    void RegisterCharacterBindings( ScriptEngine::Impl& impl )
    {
        sol::table entity = impl.Lua["Entity"];
        entity["move"]           = &ScriptEntity::Move;
        entity["jump"]           = &ScriptEntity::Jump;
        entity["crouch"]         = &ScriptEntity::Crouch;
        entity["isCrouched"]     = &ScriptEntity::IsCrouched;
        entity["isOnGround"]     = &ScriptEntity::IsOnGround;
        entity["addYaw"]         = &ScriptEntity::AddYaw;
        entity["addCameraPitch"] = &ScriptEntity::AddCameraPitch;
        entity["setSwimming"]    = &ScriptEntity::SetSwimming;
        entity["swim"]           = &ScriptEntity::Swim;
        entity["isSwimming"]     = &ScriptEntity::IsSwimming;
    }
} // namespace Desert::Scripting
