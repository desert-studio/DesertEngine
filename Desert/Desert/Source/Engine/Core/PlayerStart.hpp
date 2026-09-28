#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/ECS/Entity.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;

    // WHERE PLAY PUTS THE PLAYER — UE's GameModeBase::FindPlayerStart + SpawnDefaultPawnFor, as data.
    //
    // The level names a pawn (SceneSettings::DefaultPawn, UE's GameMode DefaultPawnClass) and places
    // PlayerStart entities; Play instantiates the pawn at the chosen start. There is NO origin fallback:
    // a pawn with nowhere to stand is a level that is wrong, and saying so is cheaper than a player who
    // appears inside the terrain at (0,0,0) and a morning spent finding out why.

    // One PlayerStart as the selection rule sees it: the entity's name (for the error text) and its tag.
    struct PlayerStartCandidate
    {
        std::string Name;
        std::string Tag;
    };

    // THE SELECTION RULE, pure so it is testable without a world.
    //   * a requested tag      -> the one start carrying it; none or several is an error naming them;
    //   * no tag, one start    -> that start, whatever its tag (UE: the only one there is);
    //   * no tag, several      -> the one UNTAGGED start (tagged starts are named entries — a door, a
    //                              checkpoint — reached by asking for them); two untagged is an error
    //                              naming both, because either answer would be a coin toss.
    [[nodiscard]] Common::ResultStr<std::size_t> ChoosePlayerStart( std::span<const PlayerStartCandidate> starts,
                                                                    std::string_view requestedTag );

    // THE VIEW-TARGET RULE (UE: APlayerCameraManager's view target), pure for the same reason:
    //   (1) a camera on the player's pawn; (2) the ONE scene camera with AutoActivateForPlayer — two or
    //   more is an error naming them; (3) the editor camera, only in Play from Here. Nothing else: Play
    //   never picks a camera by entity order, and never silently shows the editor view.
    enum class ViewTargetKind
    {
        PawnCamera,
        AutoActivateCamera, // index into the names passed in
        EditorCamera,
    };

    struct ViewTargetChoice
    {
        ViewTargetKind Kind      = ViewTargetKind::EditorCamera;
        std::size_t    AutoIndex = 0;
    };

    struct ViewTargetInputs
    {
        std::string                  Level;
        bool                         PawnSpawned   = false;
        bool                         PawnHasCamera = false;
        std::span<const std::string> AutoActivateCameras; // names of the cameras with the flag
        bool                         PlayFromHere = false;
    };

    [[nodiscard]] Common::ResultStr<ViewTargetChoice> ChooseViewTarget( const ViewTargetInputs& in );

    // How Play was asked to begin.
    struct PlayRequest
    {
        // UE's PlayerStartTag / "?StartSpot=" option: which tagged start to use. Empty = the rule above.
        std::string PlayerStartTag;
        // Play from Here: the pawn stands at this world transform (the editor camera's) instead of a
        // PlayerStart, and the editor camera becomes the last-resort view target.
        std::optional<glm::mat4> SpawnAt;
    };

    // The packaged game's `--player-start <tag>` (UE's "?StartSpot=" travel option) read from its command
    // line: every other argument is someone else's and is skipped. A flag with no tag after it, or an
    // empty tag, is refused by name - a launch that asked for a start must not silently get the default
    // one. Stated twice, it is refused too. PURE (suite PlayerStart).
    [[nodiscard]] Common::ResultStr<PlayRequest> PlayRequestFromArgs( std::span<const std::string> args );

    // Spawns the level's DefaultPawn for @p request and records it on the scene as the player's pawn.
    // Returns the pawn root, or a null entity when the level names no pawn (a level without a player —
    // a cinematic, a benchmark — is legal). Every refusal names the level, the rule and the entities.
    //
    // Nothing here is undone on Stop by hand: the editor restores the pre-Play snapshot, which never held
    // the pawn, and the packaged game has no Stop.
    [[nodiscard]] Common::ResultStr<ECS::Entity>
    SpawnDefaultPawn( Scene& scene, const Assets::AssetManager& assets, const PlayRequest& request );

    // Everything Play needs before the first gameplay tick, in the one order both the editor and the
    // packaged game use: spawn the pawn, resolve the view target, enter SceneState::Play. On failure the
    // scene is left in its state from before the call (nothing spawned stays behind) and the error says why.
    [[nodiscard]] Common::BoolResultStr BeginPlay( Scene& scene, const Assets::AssetManager& assets,
                                                   const PlayRequest& request );
} // namespace Desert::Core
