#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Assets/Common.hpp>

namespace Desert::ECS
{
    // THE LOCAL PLAYER'S INPUT CONTEXTS (GP1b; UE: the AddMappingContext calls a pawn makes on its local
    // player's UEnhancedInputLocalPlayerSubsystem at BeginPlay). When Play begins, ScriptSystem's
    // Input::LocalPlayerInput adds every context named here to the player's EnhancedInputSubsystem; scripts
    // add and remove more at run time (Lua Input.addContext / removeContext).
    struct EnhancedInputPlayerData
    {
        REFLECT()

        // The `.deinputcontext`s, HIGHEST PRIORITY FIRST: entry i is added at BasePriority + (count - 1 - i),
        // so the list reads top-down the way the stack consumes keys. The order is the priority because a
        // reflected vector holds handles, not (handle, priority) pairs, and two parallel vectors would be two
        // answers to one question.
        PROPERTY( DisplayName( "Mapping Contexts" ), Category( "Input" ), Asset<InputMappingContextAsset> )
        std::vector<Assets::AssetHandle> Contexts;

        // Where this list sits among contexts scripts add (Input.addContext's priority): the LAST entry gets
        // exactly this priority.
        PROPERTY( DisplayName( "Base Priority" ), Category( "Input" ) )
        int32_t BasePriority = 0;
    };

    struct EnhancedInputPlayerComponent
    {
        COMPONENT( Key( "EnhancedInputPlayer" ), Block( Data ), Run( ActorsAndUI ) )
        EnhancedInputPlayerData Data;
    };
} // namespace Desert::ECS
