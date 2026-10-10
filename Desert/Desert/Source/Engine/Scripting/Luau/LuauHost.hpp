#pragma once

// THE LUAU HOST — included ONLY by Engine/Scripting/Luau/*.cpp (and the suites that drive the host). The
// language-free Scripting module is ScriptEngine.hpp alone (PIMPL); this header is its Luau implementation:
// one VM, a sandboxed slot per script instance, the watchdog (LuauRuntime).
//
// BINDING ARCHITECTURE: what the ENGINE offers reaches Luau through reflection only — an entity's data by
// record key (self:component/has/add/remove), and every FUNCTION(ScriptCallable) of a reflected type
// (Engine/Libraries: Input, Audio, World, Timer, ui, loc, entity methods via ScriptMethod), bound by LuauBinder
// with no line per function. What is left here is about the language and the host itself (RegisterHostNatives:
// print-style log, World.set/get Luau values, entity valid/destroy/call, and the factory that turns a Luau
// function into a Reflection::Callable owned by the script that made it).

#include <Engine/Scripting/ScriptEngine.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/TimerManager.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Scripting/Luau/LuauBinder.hpp>
#include <Engine/Scripting/Luau/LuauRuntime.hpp>

#include <VM/include/lualib.h>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Scripting
{
    struct ScriptEngine::Impl
    {
        Core::Scene*          Scene  = nullptr;
        Assets::AssetManager* Assets = nullptr;

        /// The world every entry into script code runs in (opened as a Core::WorldContext::Scope), so a
        /// reflected library function reaches the scene without a language type.
        Core::WorldContext Context;

        /// entt handle -> the runtime slot of each of its script SLOTS (index = ScriptComponent.Scripts index;
        /// 0 = that slot holds no running script).
        std::unordered_map<uint32_t, std::vector<LuauSlot>> Slots;

        // Last OnUpdate error per (entity, slot): a runtime error repeats every frame — log it ONCE until it
        // changes or the script reloads, so the Logs panel stays readable.
        std::unordered_map<uint64_t, std::string> LastUpdateError;

        // A Luau function handed to the engine (Timer.after's callback) becomes a Callable OWNED by the (entity,
        // slot) running when it was made: CurrentOwner is set right before the engine enters script code (top
        // level, OnStart, OnUpdate, a firing callback), which is also why a callback can re-arm itself. Each load
        // of a slot bumps its generation, so a callable made by a replaced sandbox — or by a released slot — is
        // dead and never fires into code that is gone.
        uint64_t                               CurrentOwner = 0;
        std::unordered_map<uint64_t, uint32_t> Generations; // SlotKey -> loads so far

        /// Expires when the host goes: a callable that outlives it no longer touches the VM.
        std::shared_ptr<int> Lifetime = std::make_shared<int>( 0 );

        /// Shared script state of World.set/get/has (a registry table, alive as long as the VM).
        int WorldVars = LUA_NOREF;

        /// Entities a script destroyed: their slots are released by Settle, after the script call returns
        /// (a running slot's thread is never dropped under it).
        std::vector<uint32_t> PendingRelease;

        /// Calls (entity, slot)'s `function` when it defines one (success when it does not), then Settles.
        Common::BoolResultStr CallSlot( uint32_t entity, uint32_t slot, const char* function,
                                        std::span<const Reflection::Value> args );
        void                  Settle();
        void                  ReleaseEntity( uint32_t entity );

        /// Declared after the fields the VM's natives reach through Of(), so they outlive it.
        std::unique_ptr<LuauRuntime> Runtime;

        /// The world's timers (Core::WorldContext::Timers). Declared after the VM: its callables unpin their
        /// functions on destruction, while the VM still exists.
        Core::TimerManager Timers;

        static uint64_t SlotKey( uint32_t entity, uint32_t slot )
        {
            return ( static_cast<uint64_t>( entity ) << 32 ) | slot;
        }

        /// The runtime slot of (entity, slot), or 0.
        LuauSlot SlotOf( uint32_t entity, uint32_t slot ) const
        {
            auto it = Slots.find( entity );
            return ( it == Slots.end() || slot >= it->second.size() ) ? 0 : it->second[slot];
        }

        /// The host a native runs for: stored in the VM's registry by Install.
        static Impl& Of( lua_State* L );

        /// The live entity at `index` as (scene, handle) — a Luau error when it is not an entity or is gone.
        entt::entity CheckEntity( lua_State* L, int index ) const
        {
            return LuauBinder::CheckEntity( L, index ).Entity;
        }
        entt::registry& Registry() const
        {
            return Scene->GetRegistry();
        }
    };

    // ── The host's natives (see the note above) ─────────────────────────────
    void RegisterHostNatives( lua_State* L ); // log(), World.set/get/has, valid/destroy/call, the Callable factory
} // namespace Desert::Scripting
