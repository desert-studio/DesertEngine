#pragma once

#include <Engine/ECS/EntityValue.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

#include <string>

namespace Desert::Libraries
{
    /// The runtime face of an entity's AnimationComponent — UE's UAnimInstance functions a gameplay script
    /// calls (GetCurveValue, LinkAnimClassLayers, the state machine's parameters). Methods of the entity.
    struct AnimatorLibrary
    {
        REFLECT( ScriptName( "Animator" ) )

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "getAnimCurve" ),
                  Tooltip( "The curve's value on the clip playing now; nothing (logged) when absent." ) )
        static Reflection::Value GetAnimCurve( ECS::Entity entity, const std::string& name );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "isAnimNotifyStateActive" ) )
        static bool IsAnimNotifyStateActive( ECS::Entity entity, const std::string& name );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "linkAnimLayers" ) )
        static bool LinkAnimLayers( ECS::Entity entity, const std::string& path );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "unlinkAnimLayers" ) )
        static bool UnlinkAnimLayers( ECS::Entity entity, const std::string& path );

        /// ONE FUNCTION, AND THE GRAPH DECIDES THE TYPE: the parameter's declared type (the one an artist chose
        /// in the panel) says how `value` is read; a mismatch is named and refused, never coerced — a number on
        /// a Bool (0 would be true in Lua), a fraction on an Int, NaN/infinity anywhere. Queued, not written:
        /// the evaluator may not exist yet (AnimationComponent::PendingGraphParams).
        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setAnimParam" ) )
        static bool SetAnimParam( ECS::Entity entity, const std::string& name, const Reflection::Value& value );
    };
} // namespace Desert::Libraries
