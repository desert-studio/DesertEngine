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

namespace Desert::ECS
{
    // UE's APlayerStart: where Play puts the player's pawn (SceneSettings::DefaultPawn). Its transform is
    // the spawn transform; the tag lets a level have several named entries (a door, a checkpoint) that a
    // Play request asks for by name. Selection rules: Core::ChoosePlayerStart.
    struct PlayerStartData
    {
        REFLECT()

        PROPERTY( DisplayName( "Player Start Tag" ), Category( "Player Start" ),
                  Tooltip( "Empty = the level's default start. A tagged start is used only when Play asks "
                           "for its tag." ) )
        std::string Tag;
    };

    struct PlayerStartComponent
    {
        COMPONENT( Key( "PlayerStart" ), Block( Data ), Run( ActorsAndUI ) )
        PlayerStartData Data;
    };
} // namespace Desert::ECS
