#pragma once

// SAVE GAMES (GP5) — UE's USaveGame + UGameplayStatics::SaveGameToSlot / LoadGameFromSlot / DoesSaveGameExist /
// DeleteGameInSlot, with UPROPERTY(SaveGame) as PROPERTY(SaveGame).
//
// WHAT A SLOT HOLDS. The player's progress, never the level: the scene's identity (its header GUID and name), the
// player pawn's transform, and for every entity — keyed by its stable UUIDComponent — the values of the fields of
// its reflected component blocks (Core/Serialize/ReflectedComponentBlocks.hpp, the one list) that carry
// PROPERTY(SaveGame). A field without the flag is level data: a load never writes it. The pawn is its own record
// and not an entity entry, because Play spawns it from the DefaultPawn prefab with a fresh UUID every session.
//
// SCRIPT PROPERTIES. A Lua script's exposed properties are its Blueprint variables: the ones the script lists in
// `SaveGameProperties` (Scripting/ScriptProperty.hpp) are saved per entity UUID + script key + property name,
// from the slot's Properties (ScriptSystem reads them back from the running script after OnStart / OnUpdate). A
// load writes them into the slot's Properties; ScriptSystem hands those to the script env BEFORE OnStart on a
// slot that has not started (so OnStart already sees them), and before the next OnUpdate on a running one
// (savegame.load from Lua also writes them into the running envs at once). A saved property the script no
// longer marks SaveGame, or whose kind changed, is reported by name and skipped.
//
// WHAT IS FLAGGED. No engine component field carries PROPERTY(SaveGame): which state is progress is the game's
// decision, so content flags its own gameplay fields (and its scripts' properties). The engine provides the
// mechanism only.
//
// PHYSICS. A restored transform on an entity with a live physics body or character controller moves the body
// too (PhysicsWorld::TeleportBody / TeleportCharacter, velocity zeroed); writing only the TransformComponent
// would be undone by the next physics step, which writes the body's pose back over it.
//
// THE FILE. <root>/<slot>.desave for user 0 and <root>/User<N>/<slot>.desave for user N (UE's UserIndex), a JSON
// envelope `{"Format": "DesertSaveGame", "Version": N, ...}` written in the canonical text layout. A file of
// another format or another version is refused BY NAME (file, found, expected): reading a layout this build was
// not written for would apply garbage onto a live scene. Writing is atomic (FileSystem::WriteContentToFileAtomic:
// a temporary
// `<slot>.desave.tmp` renamed over the slot), so a crash mid-write leaves the previous save intact and the
// temporary is never read as a slot.
//
// THE ROOT. In the editor's Play it is <project>/Saved/SaveGames (UE: Saved/SaveGames). The shipped game's install
// folder is not writable for a player, so the Runtime host declares its root once at startup —
// GameUserDirectory(product)/SaveGames, beside machine.json (Common/Settings/MachineSettings.hpp) — through
// SetSaveGameRoot. No root and no project is an error, never the working directory.
//
// A LOAD IS NOT FATAL PER ENTITY. An entity the slot names that the scene no longer has is reported by its saved
// name and skipped; a field whose C++ type changed since the save, a field that is gone or no longer flagged, and
// a value of the wrong type are reported and skipped; everything else is applied. A slot saved in ANOTHER scene is
// refused whole (both scenes named) — the caller opens the slot's level first (SaveGameSceneOf tells which).

#include <Engine/Reflection/ReflectionTypes.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Document.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Physics
{
    class PhysicsWorld;
}

namespace Desert::Core
{
    class Scene;

    inline constexpr std::string_view SAVEGAME_FORMAT_NAME    = "DesertSaveGame";
    inline constexpr std::int64_t     SAVEGAME_FORMAT_VERSION = 1;
    inline constexpr std::string_view SAVEGAME_EXTENSION      = ".desave";

    // Which scene a slot belongs to: the .desce header GUID (its identity) and its name (for messages only).
    struct SaveGameSceneIdentity
    {
        std::string Guid;
        std::string Name;
    };

    // The reflected type a block's struct is described by. The engine asks ReflectionRegistry
    // (RegistryTypeLookup); a test passes its own so it can flag fields without touching the process-wide
    // registry.
    using ReflectedTypeLookup = std::function<const Reflection::TypeInfo*( const std::string& typeName )>;
    [[nodiscard]] const Reflection::TypeInfo* RegistryTypeLookup( const std::string& typeName );

    // The SaveGame property names of the script at `scriptPath`. The engine reads the script
    // (ScriptFileSaveGameLookup = Scripting::ReadScriptSaveGameProperties); a test passes its own.
    using ScriptSaveGameLookup =
         std::function<Common::ResultStr<std::vector<std::string>>( const std::string& scriptPath )>;
    [[nodiscard]] Common::ResultStr<std::vector<std::string>>
    ScriptFileSaveGameLookup( const std::string& scriptPath );

    // Where a save learns what is SaveGame: reflected fields (Types) and script properties (Scripts).
    struct SaveGameSchema
    {
        ReflectedTypeLookup  Types;
        ScriptSaveGameLookup Scripts;
    };
    // RegistryTypeLookup + ScriptFileSaveGameLookup.
    [[nodiscard]] SaveGameSchema EngineSaveGameSchema();

    // What a load did not apply, one line each, naming the entity / component / field. Empty = everything applied.
    struct SaveGameLoadReport
    {
        std::vector<std::string> Problems;
        std::size_t              AppliedFields = 0;
        bool                     PawnRestored  = false;
    };

    // ── One reflected struct
    // ───────────────────────────────────────────────────────────────────────────────────── The PROPERTY(SaveGame)
    // fields of the object `data` described by `type`, as a block in a slot holds them:
    // {"<field>": {"Type": "<C++ type>", "Value": v}}. Empty when no field of `type` is flagged. What every
    // component block goes through, and what a game's own reflected struct (UE: a USaveGame subclass) can use.
    [[nodiscard]] Common::Json::Object CaptureSaveGameFields( const Reflection::TypeInfo& type, const void* data );
    // Writes `fields` (as CaptureSaveGameFields wrote them) into `data`. A field that is gone, no longer flagged,
    // of another C++ type or of an unreadable value is reported in `report` as "<label>.<field> ..." and skipped.
    void ApplySaveGameFields( const Reflection::TypeInfo& type, void* data, const Common::Json::Node& fields,
                              const std::string& label, SaveGameLoadReport& report );

    // ── The document (no files) ─────────────────────────────────────────────────────────────────────────────────
    // The whole save of `registry` as the slot's JSON envelope. `pawn` may be entt::null (no pawn record).
    [[nodiscard]] Common::Json::Value CaptureSaveGame( const entt::registry& registry, entt::entity pawn,
                                                       const SaveGameSceneIdentity& scene,
                                                       const SaveGameSchema&        schema );

    // Applies `document` (already accepted by ReadSaveGameSlot, or straight from CaptureSaveGame) onto `registry`.
    // Refused whole when the envelope is not this format/version or the slot's scene GUID is not `scene`'s.
    // `physics` is the running world (null outside Play, where no body exists): an entity whose transform the
    // load changed and that has a live body / character is teleported in it. A live body with no world given is
    // reported (its transform alone was restored).
    [[nodiscard]] Common::ResultStr<SaveGameLoadReport> ApplySaveGame( entt::registry& registry, entt::entity pawn,
                                                                       const SaveGameSceneIdentity& scene,
                                                                       const Common::Json::Value&   document,
                                                                       const SaveGameSchema&        schema,
                                                                       Physics::PhysicsWorld*       physics );

    // ── Slot files under an explicit root ───────────────────────────────────────────────────────────────────────
    // The slot's file. A slot name is a file stem: empty, ".", "..", a path separator or a character Windows
    // refuses in a file name is an error naming the slot.
    [[nodiscard]] Common::ResultStr<std::filesystem::path>
    SaveGameSlotPath( const std::filesystem::path& root, std::string_view slot, std::uint32_t userIndex );

    [[nodiscard]] Common::BoolResultStr WriteSaveGameSlot( const std::filesystem::path& root,
                                                           std::string_view slot, std::uint32_t userIndex,
                                                           const Common::Json::Value& document );
    // The slot's document, refused by name when absent, unparsable, of another format or of another version.
    [[nodiscard]] Common::ResultStr<Common::Json::Value>
    ReadSaveGameSlot( const std::filesystem::path& root, std::string_view slot, std::uint32_t userIndex );
    [[nodiscard]] bool DoesSaveGameExist( const std::filesystem::path& root, std::string_view slot,
                                          std::uint32_t userIndex );
    // Deleting a slot that does not exist is an error naming it (UE returns false for it too).
    [[nodiscard]] Common::BoolResultStr DeleteGameInSlot( const std::filesystem::path& root, std::string_view slot,
                                                          std::uint32_t userIndex );
    // The slot names of `userIndex`, sorted. A missing root is no slots; a leftover `.desave.tmp` is not a slot.
    [[nodiscard]] Common::ResultStr<std::vector<std::string>> ListSaveGameSlots( const std::filesystem::path& root,
                                                                                 std::uint32_t userIndex );
    // The scene a slot was saved in, so a caller can open that level before LoadGameFromSlot.
    [[nodiscard]] Common::ResultStr<SaveGameSceneIdentity> SaveGameSceneOf( const Common::Json::Value& document );

    // ── The game's API (the root resolved as THE ROOT above) ────────────────────────────────────────────────────
    // Declared once by a host whose save root is not the project's Saved/SaveGames (the Runtime). Empty clears it.
    void                                                   SetSaveGameRoot( const std::filesystem::path& root );
    [[nodiscard]] Common::ResultStr<std::filesystem::path> SaveGameRoot();

    [[nodiscard]] SaveGameSceneIdentity SceneIdentityOf( const Scene& scene );

    [[nodiscard]] Common::BoolResultStr                 SaveGameToSlot( const Scene& scene, std::string_view slot,
                                                                        std::uint32_t userIndex );
    [[nodiscard]] Common::ResultStr<SaveGameLoadReport> LoadGameFromSlot( Scene& scene, std::string_view slot,
                                                                          std::uint32_t userIndex );
    [[nodiscard]] Common::ResultStr<bool> DoesSaveGameExist( std::string_view slot, std::uint32_t userIndex );
    [[nodiscard]] Common::BoolResultStr   DeleteGameInSlot( std::string_view slot, std::uint32_t userIndex );
    [[nodiscard]] Common::ResultStr<std::vector<std::string>> ListSaveGameSlots( std::uint32_t userIndex );
} // namespace Desert::Core
