#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Input
{
    class LocalPlayerInput;
}

namespace Desert::Scripting
{
    // The game's script host — LANGUAGE-FREE: no language type appears here (PIMPL); the one implementation is
    // the Luau plugin (Luau/LuauScriptEngine.cpp: sandboxed slots, the watchdog, compiled-once bytecode). What
    // the engine offers a script is its public reflected layer (FUNCTION/EVENT), bound by the language. The engine
    // exposes capabilities + lifecycle to scripts, so game behavior lives in hot-reloadable scripts instead of
    // compiled C++. Engine = mechanism/hot-path; script = behavior/decisions.
    //
    // `self` is the entity object: its data by record key (self:component("PointLight"), has/add/remove), its
    // verbs as native methods (destroy, call, move, ...). No Luau type appears here (PIMPL).
    class ScriptEngine final : public Core::ScriptHost
    {
    public:
        // The scene is needed so the bound entity API (move/jump/transform/...) can touch components; the asset
        // manager lets World.spawn(prefab) instantiate prefabs from script. assetManager may be null (spawn no-op).
        explicit ScriptEngine( Core::Scene* scene, Assets::AssetManager* assetManager = nullptr );
        ~ScriptEngine() override;

        ScriptEngine( const ScriptEngine& )            = delete;
        ScriptEngine& operator=( const ScriptEngine& ) = delete;

        // Runs a chunk in the console sandbox (its globals persist between calls). Returns the script error on
        // failure.
        Common::BoolResultStr RunString( const std::string& code );

        // REPL eval for the editor Lua console: runs @p code (as an expression first, then as a
        // statement), capturing everything it print()s AND the value of an expression into @p output.
        // Returns the Lua error on failure (with whatever was captured before it).
        Common::BoolResultStr EvalToString( const std::string& code, std::string& output );

        // Loads `path` into a fresh sandbox env (with `self` bound to the entity) for the entity's script SLOT.
        // An entity may run several scripts; each slot is an independent env. Re-load replaces the slot's env
        // (hot-reload). `entity` is the entt handle as a uint32; `slot` is the index in ScriptComponent.Scripts.
        Common::BoolResultStr LoadEntityScript( uint32_t entity, uint32_t slot, const std::string& path );

        // Calls a slot's OnStart() / OnUpdate(dt) if defined (no-op if not loaded).
        void CallStart( uint32_t entity, uint32_t slot );
        void CallUpdate( uint32_t entity, uint32_t slot, float dt );

        // Calls a slot's Lua function @p function( @p argument ) if defined (no-op if not loaded / not defined).
        // Dispatched when the entity's Animator crosses a named clip notify (OnAnimationNotify (instant),
        // OnAnimationNotifyBegin / OnAnimationNotifyEnd (a notify state), with the notify's name) and when a
        // level sequence Event key's CallScript action fires (its function, with the key's name).
        void CallSlotFunction( uint32_t entity, uint32_t slot, const char* function, const std::string& argument );

        // Calls a slot's @p callback (OnBeginOverlap / OnEndOverlap: UE ReceiveActorBeginOverlap) with the other
        // entity of the overlap, if defined. The other entity may already be destroyed (an End its removal
        // caused): the script gets a handle whose valid() is false.
        void CallOverlap( uint32_t entity, uint32_t slot, const char* callback, entt::registry& registry,
                          entt::entity other );

        // Calls a slot's OnHit(other, point, normal, impulse) if defined — UE ReceiveHit: this entity's body or
        // projectile struck @p other (null for a body no entity owns, a landscape tile) at world @p point;
        // @p normal points from this entity towards the other; @p impulse in kg*cm/s (0 for a projectile's
        // swept hit).
        void CallHit( uint32_t entity, uint32_t slot, entt::registry& registry, entt::entity other,
                      const glm::vec3& point, const glm::vec3& normal, float impulse );

        // Writes the slot's editor-set property values into its env's `Properties` table, so the running
        // script reads the overridden values. Call after LoadEntityScript, before OnStart.
        void ApplyProperties( uint32_t entity, uint32_t slot, const std::vector<ScriptProperty>& props );

        // The other direction, for the slot's SaveGame properties only (`names`, ReadScriptSaveGameProperties):
        // what the running script wrote to `Properties.<name>` is copied into `props` (added when the slot did
        // not list it yet), so the slot holds the game state a save captures. Call after OnStart / OnUpdate. A
        // value of another kind than a property can hold (a table, a function) is not copied.
        void ReadBackProperties( uint32_t entity, uint32_t slot, const std::vector<std::string>& names,
                                 std::vector<ScriptProperty>& props );

        // After a SaveGame load wrote the SaveGame properties into the ScriptComponents: every running slot gets
        // its loaded values NOW, so the script that loaded reads them on its next line and the read-back after
        // this OnUpdate does not copy the pre-load values over them (SaveGame.load).
        void ApplySaveGameProperties() override;

        // Calls OnUIMessage(msg) on EVERY loaded script that defines it. A UI message has no owner — a
        // button belongs to the canvas, not to a script — so it broadcasts, and each script decides what
        // (if anything) it answers. Drained from UI::UIMessageQueue once per frame by ScriptSystem.
        void BroadcastUIMessage( const std::string& message );

        // Drops ALL of an entity's slot environments (entity destroyed / component removed).
        void Release( uint32_t entity );

        // Shrinks an entity's env list to `count` slots (drops envs for removed slots). No-op if already <=.
        void TrimSlots( uint32_t entity, uint32_t count );

        // Advances the Timer.after scheduler by dt and fires due callbacks (with the scheduling
        // slot as the current owner, so a callback can re-arm itself). Call once per frame while
        // playing, after the scripts ran.
        void TickTimers( float dt );

        // THE LOCAL PLAYER'S ENHANCED INPUT (GP1b). TickPlayerInput runs once per played frame, after
        // Libraries::InputLibrary::NewFrame (its mouse delta) and before the scripts: the first call of a Play
        // session is UE's BeginPlay (the EnhancedInputPlayerComponents' contexts are added); EndPlayerInput ends
        // the session. The Input library reaches it through Core::WorldContext::PlayerInput.
        void TickPlayerInput( entt::registry& registry, float deltaSeconds );
        void EndPlayerInput();
        // The scene's GameMode events since last frame (Core/GameMode.hpp), in order: a PawnDied unpossesses the
        // player's input and calls OnPawnDied(pawn) on every script; a PlayerRestarted possesses the new pawn
        // (its input contexts) and calls OnPlayerRestarted(pawn). After TickPlayerInput, before the scripts run.
        void                     DeliverGameModeEvents( entt::registry& registry );
        Input::LocalPlayerInput& PlayerInput();

    public:
        // Public FORWARD declaration only: the definition lives in Luau/LuauHost.hpp,
        // shared by the modular binding translation units (Register*Bindings). The rest of the
        // engine still can't touch it (incomplete type).
        struct Impl;

    private:
        std::unique_ptr<Impl> m_Impl;
    };

    // Reads a script's `Properties` table (the editor-exposed schema + defaults) WITHOUT running the game —
    // loads the file in a throwaway Lua state. Used by the Details panel (Edit mode). Empty on error / none.
    std::vector<ScriptProperty> ReadScriptProperties( const std::string& path );
} // namespace Desert::Scripting
