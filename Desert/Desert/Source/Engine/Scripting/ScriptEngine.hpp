#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <cstdint>
#include <memory>
#include <optional>
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

namespace Desert::Scripting
{
    // Owns the embedded Lua runtime (via sol2). The concept: the engine exposes capabilities + lifecycle to
    // scripts written in Lua, so game behavior lives in hot-reloadable .lua files instead of compiled C++.
    // Engine = mechanism/hot-path; script = behavior/decisions.
    //
    // sol2/Lua headers are HEAVY and are confined entirely to ScriptEngine.cpp (PIMPL, like PhysicsWorld) so
    // the rest of the engine never pays their compile cost and never sees Lua types.
    class ScriptEngine
    {
    public:
        // The scene is needed so the bound entity API (move/jump/transform/...) can touch components; the asset
        // manager lets World.spawn(prefab) instantiate prefabs from script. assetManager may be null (spawn no-op).
        explicit ScriptEngine( Core::Scene* scene, Assets::AssetManager* assetManager = nullptr );
        ~ScriptEngine();

        ScriptEngine( const ScriptEngine& )            = delete;
        ScriptEngine& operator=( const ScriptEngine& ) = delete;

        // Runs a chunk of Lua immediately (boot self-test / quick eval). Returns the Lua error on failure.
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

        // Calls a slot's OnAnimationNotify(name) if defined (no-op if not loaded / not defined). Dispatched
        // when the entity's Animator crosses a named clip notify (footstep, hit-frame, ...).
        // @p callback is the Lua function called with the notify's name: OnAnimationNotify (instant),
        // OnAnimationNotifyBegin / OnAnimationNotifyEnd (a notify state).
        void CallAnimationNotify( uint32_t entity, uint32_t slot, const char* callback, const std::string& name );

        // Writes the slot's editor-set property values into its env's `Properties` table, so the running
        // script reads the overridden values. Call after LoadEntityScript, before OnStart.
        void ApplyProperties( uint32_t entity, uint32_t slot, const std::vector<ScriptProperty>& props );

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

        // Per-frame mouse delta the engine computed (cursor-capture aware), exposed to scripts as
        // Input.mouseDelta(). Set once per frame before running scripts.
        void SetFrameMouseDelta( float dx, float dy );

        // Advances the edge-detection state for Input.wasPressed() (down THIS frame, up LAST frame). Call once
        // per frame BEFORE running scripts so each key fires wasPressed() exactly on the press transition.
        void NewInputFrame();

        // A script may request cursor lock/unlock via Input.lockCursor()/showCursor(). ScriptSystem consumes the
        // pending request after running scripts and applies it (so it cooperates with the Escape toggle). Returns
        // nullopt if no script touched the cursor this frame; otherwise true=lock(capture), false=show(free).
        std::optional<bool> ConsumeCursorLockRequest();

    public:
        // Public FORWARD declaration only: the definition lives in Internal/ScriptRuntime.hpp,
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
