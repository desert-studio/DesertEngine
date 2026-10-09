#pragma once

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    class Scene;

    // THE GAME RULES OF ONE PLAYED WORLD (GP3) — UE's AGameModeBase: RestartPlayer, the pawn's death and the
    // respawn after a delay. What the level AUTHORS is in SceneSettings' "Game Mode" category (DefaultPawn =
    // DefaultPawnClass, PlayerController = PlayerControllerClass, RespawnDelay); this object is the run-time
    // half, owned by the Scene beside the player's pawn and reset with it (Scene::Clear). Core::BeginPlay
    // starts it, ECS::GameModeSystem ticks it, and ScriptSystem delivers its events (input possession and the
    // Lua hooks OnPawnDied / OnPlayerRestarted).
    //
    // THE DEATH, IN UE's ORDER. Kill unpossesses the pawn (the scene forgets it as the player's pawn, the
    // player's input stops reaching it) and LEAVES IT IN THE WORLD: the view stays on the body until the
    // restart, as UE's camera stays on a dead pawn, and the OnPawnDied hook is handed a live entity. The
    // restart clock starts only once the death was delivered (TakeEvents), so no hook can be handed an entity
    // the restart already destroyed and entt recycled. RestartPlayer destroys the body, spawns a new pawn at
    // the PlayerStart the level's rule picks (the tag Play began with), possesses it and resolves the view.

    enum class GameModeEventKind
    {
        PawnDied,        // Pawn = the dead body, still in the registry when delivered
        PlayerRestarted, // Pawn = the new, possessed pawn
    };

    struct GameModeEvent
    {
        GameModeEventKind Kind = GameModeEventKind::PawnDied;
        entt::entity      Pawn = entt::null;
    };

    // How RestartPlayer makes a pawn (UE SpawnDefaultPawnAtTransform): the hosts pass Core::SpawnPawnPrefabAt
    // over their asset manager (GameModeSystem); a test passes a function that builds the pawn by hand.
    using PawnSpawner = std::function<Common::ResultStr<entt::entity>( Scene&, const glm::mat4& at )>;

    // The world transform of the PlayerStart ChoosePlayerStart picks for @p tag among the scene's PlayerStart
    // entities (UE FindPlayerStart). Errors name the level and the starts.
    [[nodiscard]] Common::ResultStr<glm::mat4> PlayerStartTransform( const Scene& scene, std::string_view tag );

    class GameMode
    {
    public:
        // Play began: @p playerStartTag is the start every restart uses (UE's StartSpot option). A Play from
        // Here pawn still restarts at a PlayerStart — the editor camera is where Play began, not a spawn point.
        void Begin( std::string playerStartTag );
        // Forgets everything (Scene::Clear: the editor's Stop, a level load).
        void Reset();

        // UE: the pawn dies -> its controller unpossesses it -> RestartPlayer after RespawnDelay seconds
        // (SceneSettings). Only the player's pawn has a restart rule: any other entity is refused by name, and a
        // second death before the restart too. The C++ call; Lua's is gameMode.kill(entity).
        Common::BoolResultStr Kill( Scene& scene, entt::entity pawn );

        // Counts the delay down by @p deltaSeconds (game time: Play and a stepped frame) and restarts the player
        // when it has run out. Nothing to do -> success.
        Common::BoolResultStr Tick( Scene& scene, float deltaSeconds, const PawnSpawner& spawn );

        // UE RestartPlayer: destroys the dead body, spawns a pawn at the start, possesses it, resolves the view
        // target (its camera, else the level's AutoActivateForPlayer camera). Refused when the level names no
        // Default Pawn: a level without a player has nobody to restart.
        Common::BoolResultStr RestartPlayer( Scene& scene, const PawnSpawner& spawn );

        // Seconds until the restart; nullopt = no restart pending (alive, or a level without a pawn).
        [[nodiscard]] std::optional<float> RespawnRemaining() const
        {
            return m_RespawnIn;
        }

        // Events since the last call, oldest first. Taking a PawnDied starts its restart clock.
        [[nodiscard]] std::vector<GameModeEvent> TakeEvents();

    private:
        std::string                m_PlayerStartTag;
        std::optional<float>       m_RespawnIn;
        bool                       m_DeathDelivered = false;
        entt::entity               m_DeadPawn       = entt::null;
        std::vector<GameModeEvent> m_Events;
    };
} // namespace Desert::Core
